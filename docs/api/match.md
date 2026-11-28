# 模式匹配

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__match.html)

`faio::match(value, arms...)` 按参数顺序执行第一个匹配的分支，并返回该分支的结果。入口是 `<faio/common/match.hpp>`，模块入口是 `import faio.common;`。分支由 C++ lambda 或其他可调用对象实现。

## 分支接口

| 输入 | 分支 | 用法 |
| --- | --- | --- |
| `std::expected<T, E>`、`faio::expected<T>` | `ok(fn)`、`err(fn)` | 成功时把值传给 `ok`，失败时把错误传给 `err` |
| `std::expected<void, E>` | `ok(fn)`、`err(fn)` | 成功回调无参数，错误回调接收 `E` |
| `std::optional<T>` | `some(fn)`、`none(fn)` | 有值回调接收 `T`，空值回调无参数 |
| `std::variant<T...>` | `type_case<T>(fn)`、`index_case<I>(fn)` | 回调接收活动替代项；按类型或下标选择 |
| 普通值、枚举 | `case_(value, fn)` | 与给定值相等时执行 |
| 普通值、枚举 | `when(predicate, fn)` | 谓词为真时执行 |
| 任意输入 | `otherwise(fn)` | 回调接收完整输入或不接收参数，处理尚未匹配的情况 |
| expected 的成功或错误、optional 的有值分支 | `ok(predicate, fn)`、`err(predicate, fn)`、`some(predicate, fn)` | 先检查状态，再对内容执行谓词；`expected<void>` 成功状态的谓词无参数 |
| variant 的替代项 | `type_case<T>(predicate, fn)`、`index_case<I>(predicate, fn)` | 先检查替代项，再对内容执行谓词 |

```cpp
faio::expected<int> result{42};
const auto number = faio::match(
    result,
    faio::ok([](int value) { return value; }),
    faio::err([](const faio::error&) { return 0; }));

std::optional<int> selected;
const auto value = faio::match(
    selected,
    faio::some([](int number) { return number; }),
    faio::none([] { return 7; }));
```

`type_case<T>` 按 variant 声明的替代项类型精确匹配，`variant<int, const int>` 的两种类型分别由 `type_case<int>` 和 `type_case<const int>` 处理。variant 对象本身的 const 限定只影响回调参数。重复类型可以由同一个类型分支覆盖，需要区分位置时使用 `index_case<I>`。

## 穷尽检查与守卫

`expected`、`optional` 和 `variant` 的每种状态都需要不带守卫的对应分支或 `otherwise` 覆盖。普通值匹配需要 `otherwise` 提供兜底。缺少状态覆盖时，调用会在编译期报错。

守卫先以 const 借用检查内容，通过后才调用处理函数。带守卫的分支可能不匹配，因此不参与穷尽覆盖：

```cpp
const std::optional<int> selected{42};
const char* category = faio::match(
    selected,
    faio::some([](int value) { return value > 0; }, [] { return "positive"; }),
    faio::some([](int) { return "non-positive"; }),
    faio::none([] { return "absent"; }));
```

各适用分支的返回类型须完全一致，包括 `void` 和引用限定，不会自动转换到公共类型。上例的三个处理函数均返回 `const char*`。处理函数也可以返回 `void`，用于日志或其他同步操作。

## 转发与执行

左值输入按左值传给回调，const 输入保留 const，右值输入允许回调接管值。谓词检查完成前不会消费输入。只执行选中分支，回调抛出的异常直接传给调用方。

```cpp
auto owned = faio::match(
    std::optional<std::unique_ptr<int>>{std::make_unique<int>(42)},
    faio::some([](std::unique_ptr<int>&& value) { return std::move(value); }),
    faio::none([] { return std::unique_ptr<int>{}; }));
```

`match` 可用于常量表达式；匹配本身不分配内存。回调返回的引用仍由原对象的生命周期约束。复杂结构的内容判断使用谓词，解构使用 lambda 内的 C++ 结构化绑定。

variant 处于 `valueless_by_exception()` 状态时，`match` 按 `std::visit` 的规则抛出 `std::bad_variant_access`，即使提供 `otherwise` 也一样。

## 协程结果与嵌套状态

`match` 同步处理传入的值，不会自动等待任务。先通过 `co_await` 领取 `expected`，再匹配其成功或失败状态；处理函数返回任务时，调用方仍须显式等待这个任务。

等待 `stdio::in().read_line()` 后得到 `expected<optional<string>>`。外层区分 I/O 成功与失败，内层区分读到一行与 EOF：

```cpp
#include <faio/common/match.hpp>
#include <faio/stdio/stream.hpp>
#include <optional>
#include <string>

faio::task<std::string> read_status() {
  const auto result = co_await faio::stdio::in().read_line();
  co_return faio::match(
      result,
      faio::ok([](const std::optional<std::string>& line) {
        return faio::match(
            line,
            faio::some([](const std::string& text) { return std::string{"line: "} + text; }),
            faio::none([] { return std::string{"eof"}; }));
      }),
      faio::err([](const faio::error& error) {
        return std::string{"read error: "} + std::string{error.message()};
      }));
}
```

这里的回调同步生成状态文本，任务的等待点位于外层 `read_status` 中。I/O 错误、部分进度与取消的处理见[错误处理](../topics/errors.md#用模式匹配处理结果)，行读取的换行和长度限制见[标准输入输出](stdio.md#标准流)。

完整的可运行示例见 [match.cpp](../../examples/common/match.cpp)，边界用例见 [test_match.cpp](../../tests/test_match.cpp)。
