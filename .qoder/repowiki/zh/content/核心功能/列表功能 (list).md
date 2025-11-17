# 列表功能 (list)

<cite>
**本文档引用的文件**   
- [app_list.c](file://src/app_list.c)
- [apk_database.h](file://src/apk_database.h)
- [apk_query.h](file://src/apk_query.h)
- [apk_package.h](file://src/apk_package.h)
- [database.c](file://src/database.c)
- [query.c](file://src/query.c)
</cite>

## 目录
1. [简介](#简介)
2. [核心数据结构](#核心数据结构)
3. [过滤条件应用](#过滤条件应用)
4. [输出格式控制](#输出格式控制)
5. [迭代器模式使用](#迭代器模式使用)
6. [参数行为差异](#参数行为差异)
7. [性能注意事项](#性能注意事项)
8. [自定义列表过滤器开发接口](#自定义列表过滤器开发接口)

## 简介
`apk-tools` 的列表功能（list）用于遍历数据库并输出已安装或可用的软件包列表。该功能通过 `app_list.c` 文件中的实现逻辑，支持多种过滤条件和输出格式控制。用户可以通过不同的选项来筛选包列表，并根据需求调整输出的详细程度。

## 核心数据结构

### 列表上下文结构
`list_ctx` 结构体用于存储列表操作的上下文信息，包括匹配提供者、依赖关系、清单模式等标志位，以及用于存储匹配结果的哈希表和数组。

```c
struct list_ctx {
	struct apk_balloc *ba;
	struct apk_hash hash;
	struct match_array *matches;
	int verbosity;
	unsigned int match_providers : 1;
	unsigned int match_depends : 1;
	unsigned int manifest : 1;
};
```

### 匹配项结构
`match` 结构体用于存储匹配的包名和包信息。

```c
struct match {
	struct apk_name *name;
	struct apk_package *pkg;
};
```

**Section sources**
- [app_list.c](file://src/app_list.c#L20-L23)
- [app_list.c](file://src/app_list.c#L44-L52)

## 过滤条件应用

### 过滤选项解析
`list_parse_option` 函数负责解析用户提供的选项，并设置相应的过滤条件。例如，`-a` 选项用于列出所有可用包，`-I` 选项用于列出已安装包。

```c
static int list_parse_option(void *pctx, struct apk_ctx *ac, int opt, const char *optarg)
{
	struct list_ctx *ctx = pctx;
	struct apk_query_spec *qs = &ac->query;

	switch (opt) {
	case OPT_LIST_available:
		qs->filter.available = 1;
		break;
	case OPT_LIST_depends:
		ctx->match_depends = 1;
		break;
	case OPT_LIST_installed:
	installed:
		qs->filter.installed = 1;
		ac->open_flags |= APK_OPENF_NO_SYS_REPOS;
		break;
	case OPT_LIST_manifest:
		ctx->manifest = 1;
		goto installed;
	case OPT_LIST_origin:
		qs->match = BIT(APK_Q_FIELD_ORIGIN);
		break;
	case OPT_LIST_orphaned:
		qs->filter.orphaned = 1;
		break;
	case OPT_LIST_providers:
		ctx->match_providers = 1;
		break;
	case OPT_LIST_upgradeable:
		qs->filter.upgradable = 1;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}
```

### 查询匹配
`apk_query_matches` 函数用于遍历数据库中的所有包，并根据用户提供的查询条件进行匹配。该函数支持精确匹配和通配符匹配。

```c
int apk_query_matches(struct apk_ctx *ac, struct apk_query_spec *qs, struct apk_string_array *args, apk_query_match_cb match, void *pctx)
{
	char buf[PATH_MAX];
	struct apk_database *db = ac->db;
	struct match_ctx m = {
		.db = ac->db,
		.qs = qs,
		.cb = match,
		.cb_ctx = pctx,
		.ser_cb = match,
		.ser_cb_ctx = pctx,
		.ser.ops = &serialize_match,
	};
	int r, no_matches = 0;

	if (!qs->match) qs->match = BIT(APK_Q_FIELD_NAME);
	if (qs->match & ~APK_Q_FIELDS_MATCHABLE) return -ENOTSUP;

	if (qs->mode.empty_matches_all && apk_array_len(args) == 0) {
		qs->match = 0;
		return apk_hash_foreach(&db->available.names, match_name, &m);
	}
	if (qs->mode.recursive) return apk_query_recursive(ac, qs, args, match, pctx);

	// Instead of reporting all matches, report only best
	if (!qs->filter.all_matches) {
		m.cb = update_best_match;
		m.cb_ctx = &m;
	}

	apk_array_foreach_item(arg, args) {
		apk_blob_t bname, bvers;
		int op;

		m.has_matches = false;
		if ((qs->match & BIT(APK_Q_FIELD_OWNER)) && arg[0] == '/') {
			struct apk_query_match qm;
			apk_query_who_owns(db, arg, &qm, buf, sizeof buf);
			if (qm.pkg) {
				r = match(pctx, &qm);
				if (r) break;
				m.has_matches = true;
			}
		}

		if (qs->mode.search) {
			m.match_mode = MATCH_WILDCARD;
			m.q = apk_blob_fmt(buf, sizeof buf, "*%s*", arg);
			m.match = m.q.ptr;
			m.dep.op = APK_DEPMASK_ANY;
			m.dep.version = &apk_atom_null;
		} else {
			m.match_mode = strpbrk(arg, "?*") ? MATCH_WILDCARD : MATCH_EXACT;
			m.q = APK_BLOB_STR(arg);
			m.match = arg;

			if (apk_dep_parse(m.q, &bname, &op, &bvers) < 0)
				bname = m.q;

			m.q = bname;
			m.dep = (struct apk_dependency) {
				.version = apk_atomize_dup(&db->atoms, bvers),
				.op = op,
			};
		}

		if (qs->match == BIT(APK_Q_FIELD_NAME) && m.match_mode == MATCH_EXACT) {
			m.dep.name = apk_db_query_name(db, bname);
			if (m.dep.name) r = match_name(m.dep.name, &m);
		} else {
			// do full scan
			r = apk_hash_foreach(&db->available.names, match_name, &m);
			if (r) break;
		}
		if (!m.has_matches) {
			// report no match
			r = match(pctx, &(struct apk_query_match) { .query = m.q });
			if (r) break;
			if (m.match_mode == MATCH_EXACT) no_matches++;
		}
	}
	return no_matches;
}
```

**Section sources**
- [app_list.c](file://src/app_list.c#L106-L142)
- [query.c](file://src/query.c#L754-L836)

## 输出格式控制

### 打印包信息
`print_package` 函数负责根据用户的选项和详细程度来格式化输出包信息。不同的详细程度会输出不同级别的信息。

```c
static void print_package(const struct apk_database *db, const struct apk_name *name, const struct apk_package *pkg, const struct list_ctx *ctx)
{
	if (ctx->match_providers) printf("<%s> ", name->name);

	if (ctx->manifest) {
		printf("%s " BLOB_FMT "\n", pkg->name->name, BLOB_PRINTF(*pkg->version));
		return;
	}

	if (ctx->verbosity <= 0) {
		printf("%s\n", pkg->name->name);
		return;
	}

	printf(PKG_VER_FMT " " BLOB_FMT " ",
		PKG_VER_PRINTF(pkg), BLOB_PRINTF(*pkg->arch));

	if (pkg->origin->len)
		printf("{" BLOB_FMT "}", BLOB_PRINTF(*pkg->origin));
	else
		printf("{%s}", pkg->name->name);

	printf(" (" BLOB_FMT ")", BLOB_PRINTF(*pkg->license));

	if (pkg->ipkg)
		printf(" [installed]");
	else {
		const struct apk_package *u = apk_db_pkg_upgradable(db, pkg);
		if (u != NULL) printf(" [upgradable from: " PKG_VER_FMT "]", PKG_VER_PRINTF(u));
	}

	if (ctx->verbosity > 1) {
		printf("\n  " BLOB_FMT "\n", BLOB_PRINTF(*pkg->description));
		if (ctx->verbosity > 2)
			printf("  <"BLOB_FMT">\n", BLOB_PRINTF(*pkg->url));
	}

	printf("\n");
}
```

**Section sources**
- [app_list.c](file://src/app_list.c#L54-L91)

## 迭代器模式使用

### 主函数实现
`list_main` 函数是列表功能的主入口，它初始化上下文，设置查询条件，调用 `apk_query_matches` 遍历所有包，并将匹配的结果排序后输出。

```c
static int list_main(void *pctx, struct apk_ctx *ac, struct apk_string_array *args)
{
	struct apk_out *out = &ac->out;
	struct apk_database *db = ac->db;
	struct apk_query_spec *qs = &ac->query;
	struct list_ctx *ctx = pctx;

	ctx->ba = &ac->ba;
	ctx->verbosity = apk_out_verbosity(out);

	qs->mode.empty_matches_all = 1;
	qs->filter.all_matches = 1;
	if (!qs->match) {
		if (ctx->match_depends) qs->match = BIT(APK_Q_FIELD_DEPENDS);
		else if (ctx->match_providers) qs->match = BIT(APK_Q_FIELD_NAME) | BIT(APK_Q_FIELD_PROVIDES);
		else qs->match = BIT(APK_Q_FIELD_NAME);
	}

	apk_hash_init(&ctx->hash, &match_ops, 100);
	match_array_init(&ctx->matches);
	apk_query_matches(ac, qs, args, list_match_cb, ctx);
	apk_array_qsort(ctx->matches, match_array_sort);
	apk_array_foreach_item(m, ctx->matches) print_package(db, m->name, m->pkg, ctx);
	match_array_free(&ctx->matches);
	apk_hash_free(&ctx->hash);
	return 0;
}
```

**Section sources**
- [app_list.c](file://src/app_list.c#L171-L196)

## 参数行为差异

### --quiet 参数
`--quiet` 参数会减少输出的详细程度，只输出包名。

### --no-cache 参数
`--no-cache` 参数会禁用缓存，直接从远程仓库获取最新的包信息。

**Section sources**
- [app_list.c](file://src/app_list.c#L106-L142)

## 性能注意事项

### 大规模列表输出
在处理大规模列表输出时，应注意以下几点：
- 使用高效的哈希表和数组来存储匹配结果。
- 避免重复的数据库查询。
- 在输出前对结果进行排序，以提高可读性。

**Section sources**
- [app_list.c](file://src/app_list.c#L171-L196)

## 自定义列表过滤器开发接口

### 开发接口说明
开发者可以通过实现自定义的过滤器函数来扩展列表功能。这些函数需要遵循 `apk_query_match_cb` 的签名，并在 `list_main` 函数中注册。

```c
typedef int (*apk_query_match_cb)(void *pctx, struct apk_query_match *);
```

**Section sources**
- [app_list.c](file://src/app_list.c#L171-L196)
- [apk_query.h](file://src/apk_query.h#L119-L120)