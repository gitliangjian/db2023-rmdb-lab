# Repository Guidelines

## Project Structure & Module Organization

This repository contains a course implementation of RMDB. Start with `README.md` for the current implementation notes and `实践实现记录.md` for per-topic verification records. The main C++ project lives in `rmdb/`: `src/` contains the database server and modules, `rmdb_client/` contains the interactive client, and `deps/` contains vendored GoogleTest. Important module directories include `src/storage/`, `src/record/`, `src/index/`, `src/execution/`, `src/parser/`, `src/transaction/`, and `src/recovery/`. Generated build output belongs in `rmdb/build/` or `rmdb/rmdb_client/build/` and should not be committed.

## Build, Test, and Development Commands

Install the expected Linux toolchain first: `build-essential`, `cmake`, `flex`, `bison`, and `libreadline-dev`.

```bash
cd rmdb
cmake -S . -B build
cmake --build build -j
./build/bin/unit_test
./build/bin/rmdb testdb
```

Build the client separately when needed:

```bash
cd rmdb/rmdb_client
cmake -S . -B build
cmake --build build -j
./build/rmdb_client
```

## Coding Style & Naming Conventions

Use C++17 and follow the existing style: 4-space indentation in function bodies, braces on the same line for functions and control blocks, `snake_case` for functions and variables, and trailing underscores for class members such as `page_table_`. Prefer existing module boundaries and helper APIs over introducing new cross-cutting utilities. Parser grammar changes should be made in `src/parser/lex.l` and `src/parser/yacc.y`; rebuild and review regenerated `lex.yy.*` and `yacc.tab.*` files.

## Testing Guidelines

`unit_test` uses GoogleTest and currently covers core storage behavior. Add focused tests near the module under change when practical, and always run `./build/bin/unit_test` before submitting. For SQL execution, transaction, concurrency, and recovery changes, also perform manual server/client or socket-script checks and compare client output with `<db_name>/output.txt`.

## Commit & Pull Request Guidelines

Recent commits use concise, descriptive summaries, often in Chinese, for example `整理项目文档并补充块嵌套连接实现`. Keep commits scoped to one logical change. Pull requests should describe the changed RMDB subsystem, list build/test commands run, note manual SQL scenarios, and mention any generated parser files or intentionally ignored runtime artifacts.

## Security & Configuration Tips

Do not commit `rmdb/build/`, client build folders, runtime database directories, `db.log`, `output.txt`, local notes, archives, or PDFs. Database directories such as `rmdb/testdb/` are disposable run artifacts.
