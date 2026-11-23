# race_in

`race_in` 在已有任务组内竞速，胜出分支一到就返回，落选分支留给组收尾。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `race_in(group, tasks...)` | 在任务组内并行启动分支，任意分支先到达 value、error 或 stopped 即胜出，随后停止其余分支并立即返回 |
| `index` | 胜出分支在原始参数中的位置 |
| `outcome` | 同形状的 `std::variant<task_outcome<Ts>...>` |
| `loser_error(i)` | 落选分支退出后发布的错误；尚未发布时为空 |
