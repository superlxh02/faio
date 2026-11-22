"""Build the public symbol index from the named-module exports and source XML."""
from collections import defaultdict, deque
from html import escape, unescape
import json
import re
import xml.etree.ElementTree as ET


def text(node):
    return ''.join(node.itertext()).strip() if node is not None else ''


def exports(root):
    symbols = defaultdict(set)
    for path in sorted((root / 'cxx-module').rglob('*.cppm')):
        source = path.read_text(encoding='utf-8')
        module = re.search(r'^export module ([\w.]+);', source, re.M).group(1)
        guards = []
        for line in source.splitlines():
            if line.startswith('#if '):
                guards.append(line[4:].strip())
            elif line.startswith('#endif'):
                guards.pop()
            match = re.fullmatch(r'\s*using ::(faio::[^;\s]+);', line)
            if match and 'detail' not in match[1].split('::'):
                symbols[match[1]].add((module, tuple(guards)))
    return symbols


def header_routes(root):
    """Nearest public include for every reachable implementation header."""
    include = root / 'include'
    graph = {}
    for path in (include / 'faio').rglob('*'):
        if path.suffix in ('.h', '.hpp', '.inc'):
            graph[path.relative_to(include).as_posix()] = re.findall(
                r'^\s*#\s*include\s*["<](faio/[^">]+)[">]', path.read_text(encoding='utf-8'), re.M)
    routes = defaultdict(dict)
    for header in sorted(graph):
        if '/detail/' in header:
            continue
        queue = deque([(header, 0)])
        while queue:
            target, depth = queue.popleft()
            if header in routes[target]:
                continue
            routes[target][header] = depth
            queue.extend((child, depth + 1) for child in graph.get(target, ()))
    return routes


def source_nodes(model, name):
    if name in model.compounds:
        specializations = [candidate for candidate in model.compounds
                           if candidate.startswith(name + '<') and candidate.endswith('>')]
        return [model.get(name), *(model.get(candidate) for candidate in specializations)]
    namespace, short = name.rsplit('::', 1)
    if namespace not in model.compounds:
        return []
    return [item for item in model.get(namespace).findall('sectiondef/memberdef')
            if item.findtext('name') == short and item.get('prot') == 'public']


def nearest_header(routes, variants, modules, preferred_header):
    files = {node.find('location').get('file').removeprefix('include/')
             for _, nodes in variants.values() for node in nodes}
    # A module may additionally export a Windows overload that no public
    # header includes. Only advertise a header when it reaches every declaration.
    common = set.intersection(*(set(routes.get(path, {})) for path in files))
    if preferred_header in common:
        return preferred_header

    def priority(header):
        family = header.split('/')[1]
        optional = family in ('experimental', 'process', 'tls') and 'faio.' + family not in modules
        depth = max(routes[path][header] for path in files)
        return (optional, depth, header == 'faio/faio.hpp', header)

    return min(common, key=priority) if common else None


GROUPS = {'blocking': 'runtime', 'this_coro': 'runtime', 'runtime': 'runtime', 'tls': 'tls',
          'process': 'process', 'experimental': 'experimental', 'io': 'io', 'net': 'network',
          'fs': 'filesystem', 'time': 'time', 'sync': 'sync', 'log': 'errors', 'common': 'errors'}


def group_for(name, header):
    scope = name.split('::')[1]
    family = header.split('/')[1] if header else ''
    return GROUPS.get(scope, GROUPS.get(family, 'tasks'))


def page_id(name):
    # Case distinguishes types such as io::Ready from functions such as io::ready,
    # even on case-insensitive filesystems.
    encoded = re.sub(r'[A-Z]', lambda m: '_' + m[0].lower(), name.replace('::', '_'))
    return 'symbol_' + encoded.replace('|', '_or')


def code(lines):
    return ['@code{.cpp}', *lines, '@endcode']


def clean(value):
    # Keep opaque implementation constraints visibly elided instead of inventing public types.
    value = re.sub(r'(?:::)?(?:\b\w+::\s*)*\b(?:\w*_detail|detail)::\s*[\w:]+', '…', value)
    return re.sub(r'\bresource_state\b', '…', value)


def prose(value):
    return escape(clean(' '.join(value.split()))).replace('@', '&#64;').replace('\\', '&#92;')


def template(node):
    parts = []
    for param in node.findall('templateparamlist/param'):
        part = text(param.find('type'))
        if param.findtext('declname'):
            part += ' ' + param.findtext('declname') + (param.findtext('array') or '')
        if param.find('defval') is not None:
            part += ' = ' + text(param.find('defval'))
        parts.append(part)
    return ['template <' + ', '.join(parts) + '>'] if parts else []


def signatures(node, name):
    kind = node.get('kind')
    tp = text(node.find('type'))
    args = node.findtext('argsstring') or ''
    requires = text(node.find('requiresclause'))
    heading = template(node)
    if kind == 'function':
        prefix = '[[nodiscard]] ' if node.get('nodiscard') == 'yes' else ''
        if node.get('static') == 'yes' and node.get('id', '').startswith(('class', 'struct')):
            prefix += 'static '
        if node.get('explicit') == 'yes':
            prefix += 'explicit '
        if node.get('consteval') == 'yes':
            prefix += 'consteval '
        elif node.get('constexpr') == 'yes':
            prefix += 'constexpr '
        decl = prefix + (tp + ' ' if tp else '') + name + args
        if requires and 'requires' not in args:
            decl += ' requires ' + requires
    elif kind == 'typedef':
        decl = 'using ' + name + ' = ' + tp
    elif kind == 'enum':
        decl = ('enum class ' if node.get('strong') == 'yes' else 'enum ') + name
        if tp:
            decl += ' : ' + tp
        decl += ' { ' + ', '.join(v.findtext('name') + (' ' + text(v.find('initializer')) if v.find('initializer') is not None else '') for v in node.findall('enumvalue')) + ' }'
    elif kind == 'variable':
        decl = ('constexpr ' if node.get('constexpr') == 'yes' else '') + tp + ' ' + name + args
    elif kind == 'concept':
        return [clean(re.sub(r'/\*.*?\*/', '', text(node.find('initializer')), flags=re.S)).rstrip(';') + ';']
    else:
        decl = kind + ' ' + name
        if requires:
            heading.append('requires ' + requires)
    return [clean(line) for line in heading + [decl + ';']]


def describe(node):
    lines = []
    brief = text(node.find('briefdescription'))
    if brief:
        lines.append(prose(brief))
    for para in node.findall('detaileddescription/para'):
        # Nested parameter lists and return blocks are rendered with their labels below.
        parts = [para.text or '']
        for child in para:
            if child.tag not in ('parameterlist', 'simplesect'):
                parts.append(text(child))
            parts.append(child.tail or '')
        paragraph = ''.join(parts).strip()
        if paragraph and paragraph != brief:
            lines.extend(['', prose(paragraph)])
    for param in node.findall('detaileddescription//parameteritem'):
        names = [text(item) for item in param.findall('parameternamelist/parametername')]
        lines.append('- ' + ', '.join('`' + n + '`' for n in names) + ' — ' + prose(text(param.find('parameterdescription'))))
    for section in node.findall('detaileddescription//simplesect'):
        label = {'return': '返回值', 'note': '说明', 'warning': '注意', 'pre': '前置条件'}.get(section.get('kind'))
        if label:
            lines.extend(['', '**' + label + '** ' + prose(text(section))])
    return lines


def class_members(model, node, owner):
    if node.get('kind') == 'typedef':
        target = text(node.find('type')).partition('<')[0].strip()
        ref = node.find('type/ref[@kindref="compound"]')
        if ref is not None and text(ref).strip() == target:
            node = model.get(ref.get('refid'))
        else:
            choices = [compound for compound in model.compounds if compound.endswith('::' + target)]
            if len(choices) != 1:
                return
            node = model.get(choices[0])
    if node.get('kind') not in ('struct', 'class', 'union'):
        return
    own_name = node.findtext('compoundname').partition('<')[0].rsplit('::', 1)[-1]
    # Inside a class template, the injected class name and its explicitly
    # spelled specialization denote the same type in an out-of-class definition.
    parameters = []
    for param in node.findall('templateparamlist/param'):
        spelling = param.findtext('declname') or text(param.find('type')).split()[-1]
        parameters.append(re.escape(spelling))
    injected = (re.compile(r'\b' + re.escape(own_name) + r'\s*<\s*' +
                           r'\s*,\s*'.join(parameters) + r'\s*>') if parameters else None)
    members = node.findall('sectiondef/memberdef') + model.functions(node.get('id'))
    seen = set()
    for member in members:
        name = member.findtext('name')
        if (member.get('prot') != 'public' or member.get('kind') not in ('function', 'enum', 'variable', 'typedef')
                or '=delete' in (member.findtext('argsstring') or '').replace(' ', '')
                or name.startswith('~') or name in ('operator=', 'resource')):
            continue
        if member.get('kind') == 'function' and not text(member.find('type')) and name != own_name and not name.startswith('operator'):
            continue
        signature = signatures(member, owner if name == own_name else name)
        key = tuple(injected.sub(own_name, line) if injected else line for line in signature)
        if key in seen:
            continue
        seen.add(key)
        yield name, signature, member


def symbol_page(variants, name, origins, header, native):
    namespace, short = name.rsplit('::', 1)
    ident = page_id(name)
    lines = [f'@page {ident} {short} ({namespace})', '', '`' + name + '`', '']
    imports = ['import ' + module + ';' for module in sorted({module for module, guards in origins})]
    if header:
        lines.extend(['公共头文件', *code(['#include <' + header + '>'])])
    if name == 'faio::experimental::main':
        imports = ['#include <meta>', *imports, '#include <faio-module/entry.hpp>']
    lines.extend(['具名模块', *code(imports)])
    if {module for module, _ in origins} == {'faio.experimental'}:
        lines.append('使用 Linux、GCC 16.1.x、libstdc++ 16 和 C++26，启用 `FAIO_ENABLE_EXPERIMENTAL`。')
    if any('FAIO_EXPERIMENTAL_HAS_EXECUTION' in guards for _, guards in origins):
        lines.append('实验模块默认关闭 execution；使用 stdexec 桥接的应用请参阅[头文件示例](../book/api/experimental.html)。')
    if name == 'faio::experimental::main':
        lines.extend(['入口注解示例', *code(['[[= faio::experimental::main(app)]] faio::task<int> app(int argc, char** argv) {', '    co_return 0;', '}'])])
    available = set(variants)
    if len(available) < 3:
        lines.append('适用平台：' + '、'.join(variants) + '。')
    for _, guards in sorted(origins):
        for guard in guards:
            if guard.startswith('FAIO_EXPERIMENTAL_'):
                lines.append('启用条件：`' + guard + '`。')
    if native:
        lines.extend(['', f'@ref {name} "参数、返回值与使用说明"'])
    if name.startswith('faio::tls::'):
        lines.append('构建时启用 `FAIO_ENABLE_TLS`。')
    elif name.startswith('faio::process::'):
        lines.append('构建时启用 `FAIO_ENABLE_PROCESS`。')
    declarations = {}
    members = defaultdict(dict)
    for platform, (model, nodes) in variants.items():
        for node in nodes:
            compound = node.findtext('compoundname') or ''
            display_name = short + compound[compound.index('<'):] if '<' in compound else short
            sig = tuple(signatures(node, display_name))
            declarations.setdefault(sig, (node, set()))[1].add(platform)
            for method, member_sig, member in class_members(model, node, short):
                members[method].setdefault(tuple(member_sig), (member, set()))[1].add(platform)

    def append_declarations(entries):
        for sig, (node, platforms) in entries.items():
            lines.append('')
            if platforms != available:
                lines.append('适用平台：' + '、'.join(p for p in variants if p in platforms) + '。')
            lines.extend(code(sig) + describe(node))
            for value in node.findall('enumvalue'):
                lines.append('- `' + value.findtext('name') + '` ' + prose(text(value.find('briefdescription'))))

    append_declarations(declarations)
    for idx, (method, overloads) in enumerate(members.items()):
        lines.extend(['', f'@section {ident}_m{idx} {method} ({name})'])
        append_declarations(overloads)
    return lines


def write_surface(root, dest, models, curated_types, enums, report):
    symbols = exports(root)
    routes = header_routes(root)
    type_headers = {spec['public']: spec['header'] for spec in curated_types}
    curated = set(type_headers)
    curated.update(ns + '::' + name for ns, name, *_ in enums)
    curated.update(item['owner'] + '::' + item['name'] for item in report)
    groups = defaultdict(list)
    headers = defaultdict(list)
    modules = defaultdict(list)
    pages = []
    manifest = {}
    for name, origins in sorted(symbols.items()):
        imported = {module for module, guards in origins}
        variants = {}
        for platform, model in models.items():
            if imported == {'faio.experimental'} and platform != 'Linux':
                continue
            for _, guards in origins:
                if ('defined(_WIN32)' in guards and platform != 'Windows'
                        or '!defined(_WIN32)' in guards and platform == 'Windows'
                        or 'defined(__linux__)' in guards and platform != 'Linux'):
                    continue
                nodes = source_nodes(model, name)
                if not nodes:
                    raise RuntimeError(f'No {platform} source declaration for module export {name}')
                variants[platform] = (model, nodes)
        if not variants:
            raise RuntimeError('No API declaration for ' + name)
        header = nearest_header(routes, variants, imported, type_headers.get(name))
        group = group_for(name, header)
        target = page_id(name)
        link = f'@ref {target} "{name}"'
        if name in curated:
            link += f' · @ref {name} "使用说明"'
        groups[group].append(link)
        if header:
            headers[header].append(link)
        for module in imported:
            modules[module].append(link)
        lines = symbol_page(variants, name, origins, header, name in curated)
        pages.append(lines)
        sections = {line.split(' ', 2)[1]: line.split(' ', 2)[2].split(' (', 1)[0]
                    for line in lines if line.startswith('@section ')}
        manifest[name] = {'target': target, 'modules': sorted(imported), 'header': header, 'sections': sections}

    for group, links in groups.items():
        pages.append([f'@addtogroup {group}', '', '@par 模块导出', '', *['- ' + link for link in links]])
    index = ['@page public_surface 模块与头文件索引', '', '按命名模块或可包含的公共头文件查找接口。同一名称的重载在同一页列出。声明中的 … 表示省略的实现类型或约束。']
    for section, items in [('命名模块', modules), ('公共头文件', headers)]:
        index.extend(['', '## ' + section])
        for label, links in sorted(items.items()):
            index.extend(['', '### ' + label, '', *['- ' + link for link in links]])
    pages.append(index)
    dest.joinpath('surface.dox').write_text('\n\n'.join('\n'.join('/// ' + line for entry in page for line in entry.split('\n')) for page in pages), encoding='utf-8')
    dest.joinpath('surface.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')


def validate_surface(dest, html):
    manifest = json.loads(dest.joinpath('surface.json').read_text())
    pages = {c.findtext('name') for c in ET.parse(dest / 'xml/index.xml').findall('compound[@kind="page"]')}
    required = {'public_surface'} | {v['target'] for v in manifest.values() if v['target'].startswith('symbol_')}
    missing = sorted(page for page in required if page not in pages or not (html / (page + '.html')).is_file())
    if missing:
        raise RuntimeError('Missing published API pages: ' + ', '.join(missing))
    search = defaultdict(set)
    for shard in (html / 'search').glob('all_*.js'):
        # Doxygen 1.18 indexes a section by its display title and its direct URL.
        for title, target in re.findall(r"\['([^']+)',\['\.\./([^']+)'", shard.read_text(encoding='utf-8')):
            search[target].add(unescape(unescape(title)))
    count = 0
    for name, entry in manifest.items():
        namespace, short = name.rsplit('::', 1)
        page = entry['target'] + '.html'
        anchors = set(re.findall(r'\bid="([^"]+)"', (html / page).read_text(encoding='utf-8')))
        if f'{short} ({namespace})' not in search[page]:
            raise RuntimeError('Missing symbol in Doxygen search: ' + name)
        for anchor, method in entry['sections'].items():
            if anchor not in anchors or f'{method} ({name})' not in search[page + '#' + anchor]:
                raise RuntimeError(f'Missing searchable API member: {name}::{method}')
        count += len(entry['sections'])
    print(f'Doxygen search: {len(manifest)} module exports and {count} member sections verified.')
