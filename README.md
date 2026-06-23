# RMDB 课程实践实现

本仓库基于 2023 年全国大学生计算机系统能力大赛数据库管理系统设计赛提供的 RMDB 框架，按课程实践题目补全存储、查询执行、类型、索引、聚合、排序、连接、事务、并发控制和故障恢复等功能。

题目原文整理在 [实践课题目.md](实践课题目.md)，每题实现思路和验证记录整理在 [实践实现记录.md](实践实现记录.md)。

## 目录结构

```text
.
├── README.md                 # 团队协作入口文档
├── 实践课题目.md              # 当前实践题目，已按 11 题整理
├── 实践实现记录.md            # 每题需求、实现策略、关键文件和验证结果
└── rmdb/
    ├── CMakeLists.txt        # RMDB 主工程
    ├── README.md             # 原 RMDB 框架说明
    ├── License
    ├── deps/                 # googletest 等依赖
    ├── rmdb_client/          # 交互式客户端
    └── src/
        ├── analyze/          # 语义分析
        ├── common/           # 通用配置和上下文
        ├── execution/        # 执行器和执行管理
        ├── index/            # B+ 树索引
        ├── optimizer/        # 计划生成和优化
        ├── parser/           # lexer/parser/AST
        ├── record/           # 记录管理
        ├── recovery/         # WAL 和恢复
        ├── replacer/         # LRU 替换器
        ├── storage/          # 磁盘和缓冲池
        ├── system/           # 数据库和表元数据管理
        ├── transaction/      # 事务和锁管理
        ├── rmdb.cpp          # 服务端入口，TCP 端口 8765
        └── unit_test.cpp     # 存储层基础单测
```

## 已实现功能

- 存储管理：磁盘文件/page 读写、LRU 替换、BufferPool 生命周期、记录 bitmap 插删改查和全表扫描。
- 查询执行：建删表、语义检查、顺序扫描、投影、连接、insert/delete/update 执行和异常输出。
- 类型扩展：`BIGINT`、`DATETIME`，其中 `DATETIME` 使用 8 字节整数编码并严格校验日期时间。
- 唯一索引：B+ 树插查删、单列/联合唯一索引、索引扫描、DML 索引同步。
- 聚合函数：`SUM`、`MAX`、`MIN`、`COUNT(col)`、`COUNT(*)` 和 `AS` 别名。
- 排序和截断：多列 `ORDER BY`、`ASC/DESC`、`LIMIT`。
- 块嵌套循环连接：`NestedLoopJoinExecutor` 内部改为 Block Nested-Loop Join，支持非等值连接。
- 事务控制：显式 `begin/commit/abort/rollback`，隐式单语句事务自动提交，abort 回滚表记录和索引。
- 并发控制：no-wait 2PL，表级 S/X/IS/IX 和行级 S/X 锁，冲突立即 abort。
- 故障恢复：WAL、redo/undo 日志和启动恢复，恢复时同步维护索引一致性。

## 环境依赖

推荐 Linux 环境，已在 Ubuntu/GCC/CMake 工具链下验证。

```bash
sudo apt update
sudo apt install -y build-essential cmake flex bison libreadline-dev
```

项目使用 C++17。`rmdb_client` 依赖 `readline`；如果只编译服务端和单测，仍建议安装完整依赖。

## 构建

```bash
cd rmdb
cmake -S . -B build
cmake --build build -j
```

构建产物位于：

- `rmdb/build/bin/rmdb`：数据库服务端
- `rmdb/build/bin/unit_test`：基础单测
- `rmdb/build/bin/test_parser`：parser 测试程序

交互式客户端位于独立 CMake 工程中，需要时单独构建：

```bash
cd rmdb/rmdb_client
cmake -S . -B build
cmake --build build -j
```

## 运行

启动服务端：

```bash
cd rmdb
./build/bin/rmdb testdb
```

服务端监听 TCP `8765` 端口，并在当前数据库目录中生成 `output.txt`、`db.meta`、`db.log` 和表文件。数据库目录属于运行产物，已被 `.gitignore` 忽略。

另开终端启动客户端：

```bash
cd rmdb/rmdb_client
./build/rmdb_client
```

也可以用简单 socket 脚本发送 SQL，适合回归测试：

```bash
python3 - <<'PY'
import socket

sqls = [
    "create table t1 (id int, name char(8));",
    "insert into t1 values (1, 'alice');",
    "select * from t1;",
    "exit",
]

with socket.create_connection(("127.0.0.1", 8765)) as sock:
    for sql in sqls:
        sock.sendall(sql.encode() + b"\0")
        if sql == "exit":
            break
        data = sock.recv(8192).split(b"\0", 1)[0]
        print(data.decode(errors="replace"), end="")
PY
```

## 测试

运行基础单测：

```bash
cd rmdb
./build/bin/unit_test
```

常用手工验证流程：

1. 启动 `./build/bin/rmdb <db_name>`。
2. 用客户端或 socket 脚本执行题目示例 SQL。
3. 检查客户端输出和 `<db_name>/output.txt`。
4. 对事务、并发、恢复题，使用多个客户端或 crash/restart 流程验证。

最近一次验证结果见 [实践实现记录.md](实践实现记录.md)。

## 调试

Debug 构建默认开启 `-O0 -g -ggdb3`：

```bash
cd rmdb
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
gdb --args ./build/bin/rmdb debug_db
```

VS Code 可以把工作目录设为 `rmdb/`，启动目标设为 `build/bin/rmdb`，参数设为测试数据库名，例如 `debug_db`。

Parser 修改注意事项：

- 源文件是 `src/parser/lex.l` 和 `src/parser/yacc.y`。
- 项目中同时保留了生成后的 `lex.yy.cpp`、`yacc.tab.cpp/.h/.hpp`，修改语法后需要重新构建并确认生成文件已更新。

## Git 提交约定

GitHub 仓库建议只保留必要代码和 Markdown 文档：

- 保留：`rmdb/src/`、`rmdb/deps/`、`rmdb/rmdb_client/`、`rmdb/CMakeLists.txt`、`rmdb/License`、`README.md`、`实践课题目.md`、`实践实现记录.md`。
- 忽略：`rmdb/build/`、`rmdb/rmdb_client/build/`、测试数据库目录、`output.txt`、`db.log`、`.DS_Store`、压缩包、PDF 资料等本地文件。
- 不要提交运行生成的数据库目录，例如 `rmdb/testdb/`、`rmdb/*_db/`。

提交前建议检查：

```bash
git status --short
git check-ignore -v rmdb/build rmdb/testdb rmdb/BNLJ_smoke_db
```

如果之前已经把构建产物或 PDF 加入 Git 索引，需要用 `git rm --cached` 从索引移除，文件仍会留在本地。

## 许可证

原 RMDB 框架采用木兰宽松许可证第 2 版，详见 [rmdb/License](rmdb/License)。本课程实践代码在原许可证基础上维护。
