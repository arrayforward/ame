# M9 PostgreSQL 存储后端（dev-4.0）

内存图 + JSON 快照仍是默认路径（Android/bench 零变化）；本目录提供 **PostgreSQL 后端**：
节点/边规范化存表、检索下推 SQL，支撑较大数据量，不整图内存化。
编译开关 `AME_WITH_POSTGRES`（默认 OFF），依赖 libpq + 服务端 pgvector 扩展。

## 快速验证（Docker 一键）

```bash
# 启动 Docker Desktop 后，在仓库根执行：
docker compose -f storage/pg/docker-compose.yml up --abort-on-container-exit build
# db=pgvector/pgvector:pg16；build 容器装 libpq → ON 构建 → 对 db 跑 test_pg_storage
# 本机直连调试：host=localhost port=55432 dbname=ame user=postgres password=ame
```

## 使用

```cpp
ame::Engine e;
e.open_postgres("host=localhost port=55432 dbname=ame user=postgres password=ame",
                "session_42");          // ns=session 隔离；须在写入前调用
e.remember_turn("user", "...", 0);      // 之后全部读写走 PG
// 老 graph.json 迁库：e.load("graph.json") → 清 ns 后批量写库（替换式）
// 备份/导出：e.save("backup.json")（pg 模式全表导出，大库慎用）
```

bench：`--pg-conn "host=... dbname=ame user=postgres password=ame" [--pg-ns 前缀]`
（实际 ns=前缀.caseId，每 case 先清库，与内存版"全新 Engine"同语义）。

## Schema（connect 时幂等创建）

- `ame_nodes(ns,uid,kind,type,name,text,ts,precision,importance,lat,lng,geohash,
  status,mention_count,attrs jsonb,embedding vector(128), PK(ns,uid))`
- `ame_edges(ns,from_uid,to_uid,rel_type,weight,attrs jsonb, PK(ns,from_uid,to_uid,rel_type))`
- 索引：`(ns,kind,name)` / `(ns,kind)` / attrs GIN / embedding HNSW(cosine) /
  text GIN(trgm) / edges(ns,from_uid) / edges(ns,to_uid)
- 扩展：`vector`（硬依赖，ann_search 不静默退化全扫）；`pg_trgm`（可选，加速 ILIKE）。

## 检索下推对照

| 操作 | PG 实现 |
|---|---|
| find_by_name / find_all_by_name | (ns,kind,name) btree |
| find_by_attr | `attrs @> '{"key":"val"}'` JSONB GIN |
| ann_search | `embedding <=> vec` ORDER BY LIMIT（HNSW，余弦） |
| neighbors | edges(from/to) 索引；入边反向视角 + `_reversed=1` 语义不变 |
| 词法事件候选（seed 三处原全扫） | `lexical_event_candidates(words,2000)`：OR ILIKE 预过滤 + LIMIT，含同义/近义扩展词 |
| geo_lookup | kind=Place 取出后 haversine 过滤排序（Place 量小；PostGIS 留后续） |
| put_node / upsert_edge | write-through upsert；边权重累加/attrs 合并语义同内存版 |

## 与内存版的语义差异（使用必读）

1. **内存 = 常驻索引 + 有界数据缓存（LRU 换入换出）**：连接时全量装载轻量索引
   （uid→kind/name + 名字反查，每条几十字节，写时同步维护）；节点完整数据进
   LRU 缓存，上限 `AME_PG_CACHE_NODES`（默认 50000，0=不限）。到限在最外层
   调用边界按最久未用换出约 10%（**只丢内存副本，库中全量数据不动**），
   后续未命中自动从库换入；名字查找走常驻索引免 SQL，边端点存在性检查
   同样走索引。指针契约：调用内部绝不换出（本次返回的 Node* 调用期间有效），
   跨大量后续查询不应长期持有旧指针（LRU 保证最近返回者最后被淘汰）。
2. **词法 df/idf 口径**：PG 模式在**候选池内**统计 df/idf（内存版在全量 Event 上），
   候选上限 **2000**（初版 400 实测 cheap 覆盖率 0.542 低于内存 0.583，调至 2000 后
   对齐 0.583）——召回分数分布因此略有差异，属预期；sample50 对照见根 README 4.0 节。
3. **`get_node_mut` 改后必须 `put_node`**：全部既有调用点已补齐写回（7 处），
   新增代码请遵守同一约定，否则 PG 丢更新（内存模式 put_node 为同值覆盖无副作用）。
4. **快照语义**：`from_json` = 清 ns + 替换式载入；`to_json` = 全表导出（备份用）。
   `Engine::open_postgres` 本身不清库（持久化语义）；bench 每 case 显式清 ns。
5. **删 session**：`from_json(Json::object())` 即清本 ns 全部节点/边（`PgBackend::clear_ns`）。
6. **写入吞吐（已知限制）**：当前逐语句 autocommit（边写入含端点存在性检查），
   sample50 单 case 摄入约 4 分钟（内存版秒级）。批量摄入包事务 + 预检缓存是
   已识别的后续优化，不影响检索正确性。

## 租户知识库（session 隔离之上的共享层）

- **模型**：租户 = 独立 ns 的知识库引擎；session = 各自独立 ns 的私有引擎。
  不同租户的共同记忆、不同租户的 session、同租户不同 session 的私有记忆，
  在表主键/查询/索引/缓存各层全部按 ns 隔离。
- **用法**：
  ```cpp
  Engine tenant, session;
  tenant.open_postgres(conninfo, "tenant_" + tenant_id);
  session.open_postgres(conninfo, "tenant_" + tenant_id + "_session_" + session_id);
  tenant.remember_turn(...);          // 共同记忆写租户库（任何 session 都可贡献）
  session.attach_tenant(&tenant);     // 之后 session.recall 自动合并租户证据
  // session.detach_tenant() 即恢复完全私有
  ```
- **合并语义**：recall 对私有/共享分别跑完整管道，再按分数降序合并、按 uid 去重、
  截 limit；共享未命中不影响私有结果。lookup/实体档案等其它动词保持 session 私有。
- **验证**：`test_pg_storage` 租户用例断言——未 attach 不可见共享、attach 命中共享、
  其它租户共享永不可见、私有不受影响、detach 恢复隔离。

## 文件

- `pg_backend.h/.cpp`：后端实现（Storage 公开方法 `if (pg_)` 分流，调用方零改动）。
- `docker-compose.yml`：pgvector db + debian 构建容器（见上）。
- `tests/test_pg_storage.cpp`：双 ns 对照断言；无 `AME_PG_CONN` 时自动 SKIP。
