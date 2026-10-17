#!/usr/bin/env python3
"""Build Doxygen reference pages from an explicit public surface and source XML.

The first XML pass is private build state. Only documented public declarations
from the second pass are published, including methods inherited by public aliases.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET

from api_reference_content import EXPORTS, FUNCTION_GROUPS, GROUPS, ENUMS, MISSING, documentation

SOURCE = Path(__file__).resolve().parent.parent
ROOT = SOURCE.parent.parent
MODEL = ROOT / 'build/api-source'
PUBLIC = ROOT / 'build/api-public'


def text(node):
    return ''.join(node.itertext()).strip() if node is not None else ''


class SourceModel:
    def __init__(self):
        self.compounds = {}
        for item in ET.parse(MODEL / 'xml/index.xml').getroot().findall('compound'):
            name = item.findtext('name')
            self.compounds[name] = item.get('refid')
        self.cache = {}

    def get(self, name):
        ref = self.compounds.get(name, name)
        if ref not in self.cache:
            path = MODEL / 'xml' / (ref + '.xml')
            if not path.exists():
                raise RuntimeError('Doxygen source declaration not found: ' + name)
            self.cache[ref] = ET.parse(path).getroot().find('compounddef')
        return self.cache[ref]

    def functions(self, name, visited=None):
        visited = set() if visited is None else visited
        compound = self.get(name)
        if compound.get('id') in visited:
            return []
        visited.add(compound.get('id'))
        result = [member for member in compound.findall('./sectiondef/memberdef')
                  if member.get('kind') == 'function' and member.get('prot') == 'public']
        for base in compound.findall('basecompoundref'):
            if base.get('prot') == 'public' and base.get('refid'):
                result.extend(self.functions(base.get('refid'), visited))
        return result


def source_model():
    digest = hashlib.sha256(subprocess.check_output(['doxygen', '--version']))
    config = SOURCE / 'api/Doxyfile.source'
    digest.update(config.read_bytes())
    for path in sorted((ROOT / 'include/faio').rglob('*')):
        if path.is_file():
            digest.update(str(path.relative_to(ROOT)).encode())
            digest.update(path.read_bytes())
    marker = MODEL / 'fingerprint'
    if not marker.exists() or marker.read_text() != digest.hexdigest() or not (MODEL / 'xml/index.xml').exists():
        MODEL.mkdir(parents=True, exist_ok=True)
        process = subprocess.run(['doxygen', str(config)], cwd=ROOT, capture_output=True, text=True)
        (MODEL / 'generation.log').write_text(process.stdout + process.stderr, encoding='utf-8')
        if process.returncode:
            raise RuntimeError('Source XML generation failed; see build/api-source/generation.log')
        marker.write_text(digest.hexdigest())
    return SourceModel()


def public_text(value, spec):
    replacements = {
        '::faio::net::detail::': 'faio::net::', 'faio::net::detail::': 'faio::net::',
        '::faio::': 'faio::', 'detail::SocketAddr': 'SocketAddr',
        'detail::Ipv4Addr': 'Ipv4Addr', 'detail::Ipv6Addr': 'Ipv6Addr',
        'detail::HostPort': 'HostPort', 'detail::owned_native_socket': 'owned_native_socket',
    }
    for old, new in replacements.items():
        value = value.replace(old, new)
    for old, new in spec.get('bindings', {}).items():
        value = re.sub(r'\b' + re.escape(old) + r'\b', new, value)
    for alias in ('DatagramMessage', 'DatagramSend', 'OwnedDatagram'):
        value = re.sub(r'\b' + alias + r'\s*<\s*(?:faio::net::)?SocketAddr\s*>', alias, value)
    return re.sub(r'\s+', ' ', value).strip()


def template_declaration(node, spec):
    params = []
    for param in node.findall('./templateparamlist/param'):
        value = text(param.find('type'))
        name = param.findtext('declname')
        if name:
            value += ' ' + name
        default = text(param.find('defval'))
        if default:
            value += ' = ' + default
        params.append(public_text(value, spec))
    return 'template <' + ', '.join(params) + '>\n' if params else ''


def suffix(args):
    depth = 0
    for index, char in enumerate(args or ''):
        if char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
            if depth == 0:
                return args[index + 1:].strip()
    return ''


def declaration(member, spec, owner=None):
    name = member.findtext('name')
    args = member.findtext('argsstring') or ''
    if '=delete' in args.replace(' ', '') or name.startswith('~') or name == 'operator=':
        return None
    if name in spec.get('exclude', ()):
        return None
    allowed = spec.get('members')
    if allowed is not None and name not in allowed:
        return None
    constructor = name == spec['source'].split('::')[-1]
    if owner and name.startswith(('Base', 'Impl')):
        return None
    result_type = public_text(text(member.find('type')), spec)
    parameters = []
    for index, param in enumerate(member.findall('param')):
        param_type = public_text(text(param.find('type')), spec)
        param_name = param.findtext('declname')
        if not param_type or param_type == 'void':
            continue
        if not param_name:
            param_name = 'other' if owner and owner in param_type else f'argument{index + 1}'
        default = public_text(text(param.find('defval')), spec)
        parameters.append((param_type, param_name, default))
    qualifiers = public_text(suffix(args), spec)
    signature = ' '.join([result_type, name, *(p[0] for p in parameters), qualifiers])
    if re.search(r'\bdetail::|\b(?:resource_state|Socket|task_promise|handle_type|awaiter|scope_state|state)\b', signature):
        return None
    # Constructors of mixin/base implementation classes are never user factories.
    if owner and not constructor and not result_type and name != 'operator bool':
        return None
    if constructor and owner:
        name = owner
    prefix = ''
    if member.get('static') == 'yes' and owner:
        prefix += 'static '
    if member.get('explicit') == 'yes':
        prefix += 'explicit '
    if result_type:
        prefix += result_type + ' '
    values = ', '.join(t + ' ' + n + (' = ' + d if d else '') for t, n, d in parameters)
    source = template_declaration(member, spec) + prefix + name + '(' + values + ')'
    if qualifiers:
        source += ' ' + qualifiers
    return source + ';', parameters, result_type, name


def comment(spec, name, parameters, result_type, member, group=None):
    docs = documentation(spec, name, parameters, result_type, member)
    lines = ['/**', ' * @brief ' + docs['brief']]
    if group:
        lines.append(' * @ingroup ' + group)
    if docs.get('description'):
        lines += [' *', ' * ' + docs['description']]
    for _, param_name, _ in parameters:
        if param_name not in docs['params']:
            raise RuntimeError(f"Missing parameter documentation: {spec['public']}::{name}({param_name})")
        lines.append(' * @param ' + param_name + ' ' + docs['params'][param_name])
    lines += [' * ' + ('@par 返回值\n * ' if result_type in ('', 'void') else '@return ') + docs['returns']]
    if docs.get('note'):
        lines += [' * @note ' + docs['note']]
    if docs.get('errors'):
        lines += [' * @par 错误与异常', ' * ' + docs['errors']]
    if docs.get('example'):
        lines += [' * @par 使用示例', ' * @code{.cpp}']
        lines.extend(' * ' + line for line in docs['example'].splitlines())
        lines.append(' * @endcode')
    lines += [' */']
    return '\n'.join(lines)


def generate(model):
    MISSING.clear()
    PUBLIC.mkdir(parents=True, exist_ok=True)
    output = ['// Generated documentation-only declaration view. Do not compile or edit.']
    for group, title, description in GROUPS:
        output.append(f'/** @defgroup {group} {title}\n * {description}\n */')
    report = []
    nested = {}
    for spec in EXPORTS:
        if spec.get('parent'):
            nested.setdefault(spec['parent'], []).append(spec)

    def class_view(spec):
        node = model.get(spec['source'])
        short = spec['public'].split('::')[-1]
        result = [f'/** @brief {spec["description"]}\n * @ingroup {spec["group"]}\n * @par 公共头文件\n * @code{{.cpp}}\n * #include <{spec["header"]}>\n * @endcode']
        if spec.get('note'):
            result.append(' * @note ' + spec['note'])
        if spec.get('example'):
            result += [' * @par 使用示例', ' * @code{.cpp}', *[' * ' + line for line in spec['example'].splitlines()], ' * @endcode']
        result.append(' */')
        result.append(('' if spec.get('concrete') else template_declaration(node, spec)) + 'class ' + short + ' {\npublic:')
        seen = set()
        for member in model.functions(spec['source']):
            parsed = declaration(member, spec, short)
            if parsed is None:
                continue
            signature, params, result_type, name = parsed
            if signature in seen:
                continue
            seen.add(signature)
            result.append(comment(spec, name, params, result_type, member))
            result.append(signature)
            report.append({'owner':spec['public'], 'name':name, 'parameters':[p[1] for p in params], 'signature':signature})
        for member in node.findall('./sectiondef/memberdef'):
            if member.get('prot') != 'public' or member.get('kind') not in ('variable', 'typedef', 'enum'):
                continue
            name = member.findtext('name')
            if name not in spec.get('fields', {}):
                continue
            value = public_text(text(member.find('type')), spec)
            if member.get('kind') == 'enum':
                entries = []
                for entry in member.findall('enumvalue'):
                    initializer = text(entry.find('initializer'))
                    enum_name = entry.findtext('name')
                    description = spec.get('enum_values', {}).get(name, {}).get(enum_name)
                    if not description:
                        raise RuntimeError('Missing enum documentation: ' + spec['public'] + '::' + name + '::' + enum_name)
                    entries.append('/** ' + description + ' */ ' + enum_name + (' ' + initializer if initializer else ''))
                result.append('/** @brief ' + spec['fields'][name] + ' */\nenum ' + ('class ' if member.get('strong') == 'yes' else '') + name + ' {\n' + ',\n'.join(entries) + '\n};')
            elif not re.search(r'\bdetail::', value):
                result.append('/** @brief ' + spec['fields'][name] + ' */\n' + ('using ' + name + ' = ' + value if member.get('kind') == 'typedef' else value + ' ' + name) + ';')
        for child in nested.get(spec['public'], []):
            result.append(class_view(child))
        result.append('};')
        return '\n'.join(result)

    for namespace, name, group, brief, values in ENUMS:
        node = model.get(namespace)
        enum = next(m for m in node.findall('./sectiondef/memberdef') if m.get('kind') == 'enum' and m.findtext('name') == name)
        entries = []
        for entry in enum.findall('enumvalue'):
            item = entry.findtext('name')
            if item not in values:
                raise RuntimeError('Missing enum value documentation: ' + namespace + '::' + name + '::' + item)
            initializer = text(entry.find('initializer'))
            entries.append('/** ' + values[item] + ' */ ' + item + (' ' + initializer if initializer else ''))
        output.append('namespace ' + namespace + ' {\n/** @brief ' + brief + '\n * @ingroup ' + group + '\n */\nenum class ' + name + ' {\n' + ',\n'.join(entries) + '\n};\n}')
    for namespace, alias, target, group, brief in [
        ('faio','expected','std::expected<T, Error>','errors','成功值或 faio::Error；void 表示成功无业务值。'),
        ('faio','ConfigBuilder','config_builder','runtime','config_builder 的兼容名称。'),
        ('faio::fs','path','std::filesystem::path','filesystem','文件系统路径值。'),
        ('faio::net','address','SocketAddr','network','SocketAddr 的兼容名称。'),
        ('faio::net','v4addr','Ipv4Addr','network','Ipv4Addr 的兼容名称。'),
        ('faio::net','v6addr','Ipv6Addr','network','Ipv6Addr 的兼容名称。'),
    ]:
        output.append('namespace ' + namespace + ' {\n/** @brief ' + brief + '\n * @ingroup ' + group + '\n */\n' + ('template <class T>\n' if alias == 'expected' else '') + 'using ' + alias + ' = ' + target + ';\n}')
    for spec in EXPORTS:
        if spec.get('parent'):
            continue
        namespace = spec['public'].rsplit('::', 1)[0]
        output += ['namespace ' + namespace + ' {', class_view(spec), '}']
    for spec in FUNCTION_GROUPS:
        node = model.get(spec['source'])
        output.append('namespace ' + spec['public'] + ' {')
        for member in node.findall('./sectiondef/memberdef'):
            if member.get('kind') != 'function' or member.get('prot') != 'public':
                continue
            parsed = declaration(member, spec)
            if parsed is None:
                continue
            signature, params, result_type, name = parsed
            output += [comment(spec, name, params, result_type, member, spec['group']), signature]
            report.append({'owner':spec['public'], 'name':name, 'parameters':[p[1] for p in params], 'signature':signature})
        output.append('}')
    generated = '\n\n'.join(output)
    if re.search(r'\bdetail::|include/faio/detail', generated):
        raise RuntimeError('Implementation type leaked into public declaration view')
    if MISSING:
        raise RuntimeError('Missing API documentation:\n' + '\n'.join(sorted(MISSING)))
    if 'UNRESOLVED' in generated:
        raise RuntimeError('Incomplete API documentation')
    (PUBLIC / 'public.hpp').write_text(generated, encoding='utf-8')
    periodic = model.get('faio::time::detail::Interval')
    periodic_docs = {
        'tick':('等待下一个周期触发。','co_await 后无业务返回值；遵循当前 MissedTickBehavior 推进下次 deadline。',{}),
        'period':('读取固定周期。','返回 chrono::nanoseconds。',{}),
        'reset':('从当前时间重新计时。','无返回值；下次 tick 在一个 period 之后。',{}),
        'reset_immediately':('让下次 tick 立即就绪。','无返回值。',{}),
        'reset_after':('在相对时长后触发下一次。','无返回值。',{'after':'从当前时间起计算的纳秒时长。'}),
        'reset_at':('设置下次触发的绝对时间。','无返回值。',{'deadline':'steady_clock 的绝对时间点。'}),
        'missed_tick_behavior':('读取错过触发时的策略。','返回 MissedTickBehavior；初始为 Burst。',{}),
        'set_missed_tick_behavior':('替换错过触发时的策略。','无返回值。',{'behavior':'Burst、Delay 或 Skip。'}),
    }
    page = ['/** @page periodic_timer 周期定时器返回对象', ' * @ingroup time', ' * interval / interval_at 的返回对象用 auto 保存，无需命名其类型。', ' * @code{.cpp}', ' * auto timer = faio::time::interval(std::chrono::seconds{1});', ' * co_await timer.tick();', ' * @endcode']
    found = set()
    for member in periodic.findall('./sectiondef/memberdef'):
        name = member.findtext('name')
        if member.get('kind') != 'function' or member.get('prot') != 'public' or name == 'Interval':
            continue
        if name not in periodic_docs:
            raise RuntimeError('Missing periodic timer documentation: ' + name)
        found.add(name)
        brief, returns, params = periodic_docs[name]
        spec = {'source':'faio::time::detail::Interval','public':'周期定时器返回对象'}
        signature, arguments, _, _ = declaration(member, spec, 'timer')
        signature = re.sub(r'\bSleep\b', 'auto', signature)
        page += [' * @section periodic_' + name + ' ' + name + '()', ' * ' + brief, ' * @code{.cpp}', ' * ' + signature, ' * @endcode']
        for _, parameter, _ in arguments:
            page += [' * - **' + parameter + '**：' + params[parameter]]
        page += [' * @par 返回值', ' * ' + returns]
    if found != set(periodic_docs):
        raise RuntimeError('Periodic timer member coverage changed')
    page.append(' */')
    (PUBLIC / 'periodic.dox').write_text('\n'.join(page), encoding='utf-8')
    (PUBLIC / 'coverage.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


def validate_reference(report, output):
    members = {}
    for path in (PUBLIC / 'xml').glob('*.xml'):
        if path.name == 'index.xml':
            continue
        for member in ET.parse(path).getroot().findall('.//memberdef[@kind="function"]'):
            if member.get('prot') != 'public':
                continue
            members[member.get('id')] = member
    if len(members) != len(report):
        raise RuntimeError(f'Doxygen declaration coverage changed: {len(members)} / {len(report)}')
    for member in members.values():
        detail = member.find('detaileddescription')
        names = {p.findtext('declname') for p in member.findall('param') if p.findtext('declname')}
        documented = {p.text for p in detail.findall('.//parametername')}
        returns = detail.findall('.//simplesect[@kind="return"]') + [s for s in detail.findall('.//simplesect[@kind="par"]') if text(s.find('title')) == '返回值']
        if names - documented or not returns or not text(member.find('briefdescription')):
            raise RuntimeError('Incomplete generated Doxygen member: ' + member.findtext('definition'))
    for path in (output / 'api').rglob('*'):
        if path.suffix not in ('.html', '.js'):
            continue
        source = path.read_text(encoding='utf-8')
        if re.search(r'detail(?:::|/|_1_1)|resource_state|UNRESOLVED', source):
            raise RuntimeError('Implementation symbol leaked into API reference: ' + path.name)
        if path.name.endswith('_source.html') or path.name == 'files.html':
            raise RuntimeError('Source index leaked into API reference: ' + path.name)


def build_reference(output):
    model = source_model()
    report = generate(model)
    shutil.rmtree(PUBLIC / 'xml', ignore_errors=True)
    subprocess.run(['doxygen', str(SOURCE / 'api/Doxyfile')], cwd=ROOT, check=True)
    validate_reference(report, output)
    nav = '<nav class="faio-api-nav" aria-label="faio 导航"><a class="brand" href="../"><img src="../assets/favicon.svg" alt="">faio</a><a href="../book/">指南</a><a href="index.html" aria-current="page">API 参考</a><a href="../book/examples/">官方例程</a><a href="https://github.com/superlxh02/faio">GitHub ↗</a></nav>'
    for page in (output / 'api').glob('*.html'):
        source = page.read_text(encoding='utf-8')
        source = source.replace('<div id="top">', '<div id="top">' + nav, 1)
        # Doxygen 1.18 emits a spurious underscore member-index filename.
        source = source.replace('functions__.html#index__', 'functions.html#index__5F')
        if page.name.startswith('functions'):
            # Doxygen omits letter anchors on short member indexes.
            for letter in set(re.findall(r'href="#index_([a-z])"', source)):
                anchor = 'index_' + letter
                if 'id="' + anchor + '"' not in source:
                    source = re.sub(r'<li>(?=' + re.escape(letter) + ')', '<li id="' + anchor + '">', source, count=1, flags=re.IGNORECASE)
        page.write_text(source, encoding='utf-8')
    print(f'Doxygen: {len(EXPORTS)} public types, {len(report)} detailed declarations.', flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', action='store_true', help='Generate and validate the published HTML reference')
    args = parser.parse_args()
    if args.build:
        build_reference(ROOT / 'build/docs-staging')
    else:
        generate(source_model())
