/*
 * Copyright 2024 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rspamadm.h"
#include "cfg_file.h"
#include "cfg_rcl.h"
#include "utlist.h"
#include "rspamd.h"
#include "lua/lua_common.h"
#include "utlist.h"

static gboolean json = FALSE;
static gboolean compact = FALSE;
static gboolean show_help = FALSE;
static gboolean show_comments = FALSE;
static gboolean modules_state = FALSE;
static gboolean symbol_groups_only = FALSE;
static gboolean symbol_full_details = FALSE;
static gboolean skip_template = FALSE;
static char *config = NULL;
extern struct rspamd_main *rspamd_main;
/* Defined in modules.c */
extern module_t *modules[];
extern worker_t *workers[];

static void rspamadm_configdump(int argc, char **argv, const struct rspamadm_command *);
static const char *rspamadm_configdump_help(gboolean full_help, const struct rspamadm_command *);
/*Add local_only option to GOptionEntry entries array */
static gboolean local_only = FALSE;



struct rspamadm_command configdump_command = {
	.name = "configdump",
	.flags = 0,
	.help = rspamadm_configdump_help,
	.run = rspamadm_configdump,
	.lua_subrs = NULL,
};

static GOptionEntry entries[] = {
	{"json", 'j', 0, G_OPTION_ARG_NONE, &json,
	 "Json output (pretty formatted)", NULL},
	{"compact", 'C', 0, G_OPTION_ARG_NONE, &compact,
	 "Compacted json output", NULL},
	{"config", 'c', 0, G_OPTION_ARG_STRING, &config,
	 "Config file to test", NULL},
	{"show-help", 'h', 0, G_OPTION_ARG_NONE, &show_help,
	 "Show help as comments for each option", NULL},
	{"show-comments", 's', 0, G_OPTION_ARG_NONE, &show_comments,
	 "Show saved comments from the configuration file", NULL},
	{"modules-state", 'm', 0, G_OPTION_ARG_NONE, &modules_state,
	 "Show modules state only", NULL},
	{"groups", 'g', 0, G_OPTION_ARG_NONE, &symbol_groups_only,
	 "Show symbols groups only", NULL},
	{"symbol-details", 'd', 0, G_OPTION_ARG_NONE, &symbol_full_details,
	 "Show full symbol details only", NULL},
	{"skip-template", 'T', 0, G_OPTION_ARG_NONE, &skip_template,
	 "Do not apply Jinja templates", NULL},
	 {"local-only", 'l', 0, G_OPTION_ARG_NONE, &local_only,
	"Show only local configuration elements (priority > 0)",NULL},
	{NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}};

static const char *
rspamadm_configdump_help(gboolean full_help, const struct rspamadm_command *cmd)
{
	const char *help_str;

	if (full_help) {
		help_str = "Perform configuration file dump\n\n"
				   "Usage: rspamadm configdump [-c <config_name> [-j --compact -m] [<path1> [<path2> ...]]]\n"
				   "Where options are:\n\n"
				   "-j: output plain json\n"
				   "--compact: output compacted json\n"
				   "-c: config file to test\n"
				   "-m: show state of modules only\n"
				   "-h: show help for dumped options\n"
				   "-L: show only local configuration (priority > 0)\n"
				   "--help: shows available options and commands";
	}
	else {
		help_str = "Perform configuration file dump";
	}

	return help_str;
}

// Function to filter local-only configuration elements

static ucl_object_t *
rspamadm_filter_local_config(const ucl_object_t *obj)
{
	ucl_object_t *result = NULL;
	const ucl_object_t *cur;
	ucl_object_iter_t it = NULL;
	gboolean has_local = FALSE;

	if (obj == NULL) {
		return NULL;
	}

	switch (ucl_object_type(obj)) {
	case UCL_OBJECT:
		result = ucl_object_typed_new(UCL_OBJECT);

		while ((cur = ucl_object_iterate(obj, &it, true))) {
			if (cur->priority > 0) {
				// This is a local configuration element
				ucl_object_insert_key(result, ucl_object_ref(cur), cur->key, cur->keylen, true);
				has_local = TRUE;
			}
			else {
				// Check whether children is local
				ucl_object_t *filtered_child = NULL;

				if (ucl_object_type(cur) == UCL_OBJECT || ucl_object_type(cur) == UCL_ARRAY) {
					filtered_child = rspamadm_filter_local_config(cur);
					if (filtered_child != NULL) {
						ucl_object_insert_key(result, filtered_child, cur->key, cur->keylen, true);
						has_local = TRUE;
					}
				}
			}
		}

		if (!has_local) {
			ucl_object_unref(result);
			return NULL;
		}
		break;

	case UCL_ARRAY: {
		gboolean has_local_in_array = FALSE;
		result = ucl_object_typed_new(UCL_ARRAY);

		it = NULL;
		while ((cur = ucl_object_iterate(obj, &it, true))) {
			if (cur->priority > 0) {
				// local element
				ucl_array_append(result, ucl_object_ref(cur));
				has_local_in_array = TRUE;
			}
			else {
				// Check whether childrens are local
				ucl_object_t *filtered_child = NULL;

				if (ucl_object_type(cur) == UCL_OBJECT || ucl_object_type(cur) == UCL_ARRAY) {
					filtered_child = rspamadm_filter_local_config(cur);
					if (filtered_child != NULL) {
						ucl_array_append(result, filtered_child);
						has_local_in_array = TRUE;
					}
				}
			}
		}

		if (!has_local_in_array) {
			ucl_object_unref(result);
			return NULL;
		}
		break;
	}

	default:
		if (obj->priority > 0) {
			// Handle primitive types (string, number, boolean, etc.)
			result = ucl_object_ref((ucl_object_t *)obj);
		}
		break;
	}

	return result;
}

static void
config_logger(rspamd_mempool_t *pool, gpointer ud)
{
}

static void
rspamadm_add_doc_elt(const ucl_object_t *obj, const ucl_object_t *doc_obj,
					 ucl_object_t *comment_obj)
{
	rspamd_fstring_t *comment = rspamd_fstring_new();
	const ucl_object_t *elt;
	ucl_object_t *nobj, *cur_comment;

	if (ucl_object_lookup_len(comment_obj, (const char *) &obj,
							  sizeof(void *))) {
		rspamd_fstring_free(comment);
		/* Do not rewrite the existing comment */
		return;
	}

	if (doc_obj != NULL) {
		/* Create doc comment */
		nobj = ucl_object_fromstring_common("/*", 0, 0);
	}
	else {
		rspamd_fstring_free(comment);
		return;
	}

	/* We create comments as a list of parts */
	elt = ucl_object_lookup(doc_obj, "data");
	if (elt) {
		rspamd_printf_fstring(&comment, " * %s", ucl_object_tostring(elt));
		cur_comment = ucl_object_fromstring_common(comment->str, comment->len, 0);
		rspamd_fstring_erase(comment, 0, comment->len);
		DL_APPEND(nobj, cur_comment);
	}

	elt = ucl_object_lookup(doc_obj, "type");
	if (elt) {
		rspamd_printf_fstring(&comment, " * Type: %s", ucl_object_tostring(elt));
		cur_comment = ucl_object_fromstring_common(comment->str, comment->len, 0);
		rspamd_fstring_erase(comment, 0, comment->len);
		DL_APPEND(nobj, cur_comment);
	}

	elt = ucl_object_lookup(doc_obj, "required");
	if (elt) {
		rspamd_printf_fstring(&comment, " * Required: %s",
							  ucl_object_toboolean(elt) ? "true" : "false");
		cur_comment = ucl_object_fromstring_common(comment->str, comment->len, 0);
		rspamd_fstring_erase(comment, 0, comment->len);
		DL_APPEND(nobj, cur_comment);
	}

	cur_comment = ucl_object_fromstring(" */");
	DL_APPEND(nobj, cur_comment);
	rspamd_fstring_free(comment);

	ucl_object_insert_key(comment_obj, ucl_object_ref(nobj),
						  (const char *) &obj,
						  sizeof(void *), true);

	ucl_object_unref(nobj);
}

static void
rspamadm_gen_comments(const ucl_object_t *obj, const ucl_object_t *doc_obj,
					  ucl_object_t *comments)
{
	const ucl_object_t *cur_obj, *cur_doc, *cur_elt;
	ucl_object_iter_t it = NULL;

	if (obj == NULL || doc_obj == NULL) {
		return;
	}

	if (obj->keylen > 0) {
		rspamadm_add_doc_elt(obj, doc_obj, comments);
	}

	if (ucl_object_type(obj) == UCL_OBJECT) {
		while ((cur_obj = ucl_object_iterate(obj, &it, true))) {
			cur_doc = ucl_object_lookup_len(doc_obj, cur_obj->key,
											cur_obj->keylen);

			if (cur_doc != NULL) {
				LL_FOREACH(cur_obj, cur_elt)
				{
					if (ucl_object_lookup_len(comments, (const char *) &cur_elt,
											  sizeof(void *)) == NULL) {
						rspamadm_gen_comments(cur_elt, cur_doc, comments);
					}
				}
			}
		}
	}
}

static void
rspamadm_dump_section_obj(struct rspamd_config *cfg,
						  const ucl_object_t *obj, const ucl_object_t *doc_obj)
{
	rspamd_fstring_t *output;
	ucl_object_t *comments = NULL;

	output = rspamd_fstring_new();

	if (show_help) {
		if (show_comments) {
			comments = cfg->config_comments;
		}
		else {
			comments = ucl_object_typed_new(UCL_OBJECT);
		}

		rspamadm_gen_comments(obj, doc_obj, comments);
	}
	else if (show_comments) {
		comments = cfg->config_comments;
	}

	if (json) {
		rspamd_ucl_emit_fstring_comments(obj, UCL_EMIT_JSON, &output, comments);
	}
	else if (compact) {
		rspamd_ucl_emit_fstring_comments(obj, UCL_EMIT_JSON_COMPACT, &output,
										 comments);
	}
	else {
		rspamd_ucl_emit_fstring_comments(obj, UCL_EMIT_CONFIG, &output,
										 comments);
	}

	rspamd_printf("%V", output);
	rspamd_fstring_free(output);

	if (comments != NULL) {
		ucl_object_unref(comments);
	}
}

__attribute__((noreturn)) static void
rspamadm_configdump(int argc, char **argv, const struct rspamadm_command *cmd)
{
	GOptionContext *context;
	GError *error = NULL;
	const char *confdir;
	const ucl_object_t *obj = NULL, *cur, *doc_obj;
	struct rspamd_config *cfg = rspamd_main->cfg;
	gboolean ret = TRUE;
	worker_t **pworker;
	int i;

	context = g_option_context_new(
		"configdump - dumps Rspamd configuration");
	g_option_context_set_summary(context,
								 "Summary:\n  Rspamd administration utility version " RVERSION
								 "\n  Release id: " RID);
	g_option_context_add_main_entries(context, entries, NULL);

	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		fprintf(stderr, "option parsing failed: %s\n", error->message);
		g_error_free(error);
		g_option_context_free(context);
		exit(EXIT_FAILURE);
	}

	g_option_context_free(context);

	if (config == NULL) {
		if ((confdir = g_hash_table_lookup(ucl_vars, "CONFDIR")) == NULL) {
			confdir = RSPAMD_CONFDIR;
		}

		config = g_strdup_printf("%s%c%s", confdir, G_DIR_SEPARATOR,
								 "rspamd.conf");
	}

	pworker = &workers[0];
	while (*pworker) {
		/* Init string quarks */
		(void) g_quark_from_static_string((*pworker)->name);
		pworker++;
	}

	cfg->compiled_modules = modules;
	cfg->compiled_workers = workers;
	cfg->cfg_name = config;

	if (!rspamd_config_read(cfg, cfg->cfg_name, config_logger, rspamd_main,
							ucl_vars, skip_template, lua_env)) {
		ret = FALSE;
	}
	else {
		/* Do post-load actions */
		rspamd_lua_post_load_config(cfg);

		(void) rspamd_init_filters(rspamd_main->cfg, false, false);
		rspamd_config_post_load(cfg, RSPAMD_CONFIG_INIT_SYMCACHE);
	}

	if (ret) {
		if (modules_state) {

			rspamadm_execute_lua_ucl_subr(argc,
										  argv,
										  cfg->cfg_ucl_obj,
										  "plugins_stats",
										  FALSE);

			exit(EXIT_SUCCESS);
		}

		if (symbol_full_details) {
			/*
			 * Create object from symbols groups and output it using the
			 * specified format
			 */
			ucl_object_t *out = ucl_object_typed_new(UCL_OBJECT);
			GHashTableIter it;
			gpointer sk, sv;

			g_hash_table_iter_init(&it, cfg->symbols);
			ucl_object_t *sym_ucl = ucl_object_typed_new(UCL_OBJECT);
			const ucl_object_t *all_symbols_ucl = ucl_object_lookup(cfg->cfg_ucl_obj, "symbols");

			while (g_hash_table_iter_next(&it, &sk, &sv)) {
				const char *sym_name = (const char *) sk;
				struct rspamd_symbol *s = (struct rspamd_symbol *) sv;
				ucl_object_t *this_sym_ucl = ucl_object_typed_new(UCL_OBJECT);

				ucl_object_insert_key(this_sym_ucl,
									  ucl_object_fromdouble(s->score),
									  "score", strlen("score"),
									  false);

				ucl_object_insert_key(this_sym_ucl,
									  ucl_object_fromstring(s->description),
									  "description", strlen("description"), false);

				rspamd_symcache_get_symbol_details(cfg->cache, sym_name, this_sym_ucl);

				ucl_object_insert_key(this_sym_ucl,
									  ucl_object_frombool(!!(s->flags & RSPAMD_SYMBOL_FLAG_DISABLED)),
									  "disabled", strlen("disabled"),
									  false);

				if (s->nshots == 1) {
					ucl_object_insert_key(this_sym_ucl,
										  ucl_object_frombool(true),
										  "one_shot", strlen("one_shot"),
										  false);
				}
				else {
					ucl_object_insert_key(this_sym_ucl,
										  ucl_object_frombool(false),
										  "one_shot", strlen("one_shot"),
										  false);
				}

				if (s->gr != NULL) {
					struct rspamd_symbols_group *gr = s->gr;
					const char *gr_name = gr->name;
					if (strcmp(gr_name, "ungrouped") != 0) {
						ucl_object_insert_key(this_sym_ucl,
											  ucl_object_fromstring(gr_name),
											  "group", strlen("group"),
											  false);
					}

					if (s->groups) {
						ucl_object_t *add_groups = ucl_object_typed_new(UCL_ARRAY);
						unsigned int j;
						struct rspamd_symbols_group *add_gr;
						bool has_extra_groups = false;

						PTR_ARRAY_FOREACH(s->groups, j, add_gr)
						{
							if (add_gr->name && strcmp(add_gr->name, gr_name) != 0) {
								ucl_array_append(add_groups,
												 ucl_object_fromstring(add_gr->name));
								has_extra_groups = true;
							}
						}

						if (has_extra_groups == true) {
							ucl_object_insert_key(this_sym_ucl,
												  add_groups,
												  "groups", strlen("groups"),
												  false);
						}
					}
				}

				const ucl_object_t *loaded_symbol_ucl = ucl_object_lookup(all_symbols_ucl, sym_name);
				if (loaded_symbol_ucl) {
					ucl_object_iter_t it = NULL;
					while ((cur = ucl_iterate_object(loaded_symbol_ucl, &it, true)) != NULL) {
						const char *key = ucl_object_key(cur);
						/* If this key isn't something we have direct in the symbol item, grab the key/value */
						if ((strcmp(key, "score") != 0) &&
							(strcmp(key, "description") != 0) &&
							(strcmp(key, "disabled") != 0) &&
							(strcmp(key, "condition") != 0) &&
							(strcmp(key, "one_shot") != 0) &&
							(strcmp(key, "any_shot") != 0) &&
							(strcmp(key, "nshots") != 0) &&
							(strcmp(key, "one_param") != 0) &&
							(strcmp(key, "priority") != 0)) {
							ucl_object_insert_key(this_sym_ucl, (ucl_object_t *) cur, key, strlen(key), false);
						}
					}
				}

				ucl_object_insert_key(sym_ucl, this_sym_ucl, sym_name,
									  strlen(sym_name), true);
			}
			ucl_object_insert_key(out, sym_ucl, "symbols",
								  strlen("symbols"), true);

			rspamadm_dump_section_obj(cfg, out, NULL);
			exit(EXIT_SUCCESS);
		}

		if (symbol_groups_only) {
			/*
			 * Create object from symbols groups and output it using the
			 * specified format
			 */
			ucl_object_t *out = ucl_object_typed_new(UCL_OBJECT);
			GHashTableIter it;
			gpointer k, v;

			g_hash_table_iter_init(&it, cfg->groups);

			while (g_hash_table_iter_next(&it, &k, &v)) {
				const char *gr_name = (const char *) k;
				struct rspamd_symbols_group *gr = (struct rspamd_symbols_group *) v;
				ucl_object_t *gr_ucl = ucl_object_typed_new(UCL_OBJECT);

				ucl_object_insert_key(gr_ucl,
									  ucl_object_frombool(!!(gr->flags & RSPAMD_SYMBOL_GROUP_PUBLIC)),
									  "public", strlen("public"), false);
				ucl_object_insert_key(gr_ucl,
									  ucl_object_frombool(!!(gr->flags & RSPAMD_SYMBOL_GROUP_DISABLED)),
									  "disabled", strlen("disabled"), false);
				ucl_object_insert_key(gr_ucl,
									  ucl_object_frombool(!!(gr->flags & RSPAMD_SYMBOL_GROUP_ONE_SHOT)),
									  "one_shot", strlen("one_shot"), false);
				ucl_object_insert_key(gr_ucl,
									  ucl_object_fromdouble(gr->max_score),
									  "max_score", strlen("max_score"), false);
				ucl_object_insert_key(gr_ucl,
									  ucl_object_fromdouble(gr->min_score),
									  "min_score", strlen("min_score"), false);
				ucl_object_insert_key(gr_ucl,
									  ucl_object_fromstring(gr->description),
									  "description", strlen("description"), false);

				if (gr->symbols) {
					GHashTableIter sit;
					gpointer sk, sv;

					g_hash_table_iter_init(&sit, gr->symbols);
					ucl_object_t *sym_ucl = ucl_object_typed_new(UCL_OBJECT);

					while (g_hash_table_iter_next(&sit, &sk, &sv)) {
						const char *sym_name = (const char *) sk;
						struct rspamd_symbol *s = (struct rspamd_symbol *) sv;
						ucl_object_t *spec_sym = ucl_object_typed_new(UCL_OBJECT);

						ucl_object_insert_key(spec_sym,
											  ucl_object_fromdouble(s->score),
											  "score", strlen("score"),
											  false);
						ucl_object_insert_key(spec_sym,
											  ucl_object_fromstring(s->description),
											  "description", strlen("description"), false);
						ucl_object_insert_key(spec_sym,
											  ucl_object_frombool(!!(s->flags & RSPAMD_SYMBOL_FLAG_DISABLED)),
											  "disabled", strlen("disabled"),
											  false);

						if (s->nshots == 1) {
							ucl_object_insert_key(spec_sym,
												  ucl_object_frombool(true),
												  "one_shot", strlen("one_shot"),
												  false);
						}
						else {
							ucl_object_insert_key(spec_sym,
												  ucl_object_frombool(false),
												  "one_shot", strlen("one_shot"),
												  false);
						}

						ucl_object_t *add_groups = ucl_object_typed_new(UCL_ARRAY);
						unsigned int j;
						struct rspamd_symbols_group *add_gr;

						PTR_ARRAY_FOREACH(s->groups, j, add_gr)
						{
							if (add_gr->name && strcmp(add_gr->name, gr_name) != 0) {
								ucl_array_append(add_groups,
												 ucl_object_fromstring(add_gr->name));
							}
						}

						ucl_object_insert_key(spec_sym,
											  add_groups,
											  "extra_groups", strlen("extra_groups"),
											  false);

						ucl_object_insert_key(sym_ucl, spec_sym, sym_name,
											  strlen(sym_name), true);
					}

					ucl_object_insert_key(gr_ucl, sym_ucl, "symbols",
										  strlen("symbols"), false);
				}

				ucl_object_insert_key(out, gr_ucl, gr_name, strlen(gr_name),
									  true);
			}

			rspamadm_dump_section_obj(cfg, out, NULL);

			exit(EXIT_SUCCESS);
		}

		/* Output configuration */
		if (local_only) {
			/* Filter the configuration to include only local elements */
			ucl_object_t *local_config = rspamadm_filter_local_config(cfg->cfg_ucl_obj);
			
			if (local_config == NULL) {
				rspamd_printf("No local configuration found\n");
				exit(EXIT_SUCCESS);
			}

			if (argc == 1) {
				/* Output the entire filtered configuration */
				rspamadm_dump_section_obj(cfg, local_config, cfg->doc_strings);
				ucl_object_unref(local_config);
			}

			else {
				/* Output specific sections from the filtered configuration */
				for (i = 1; i < argc; i++) {
					obj = ucl_object_lookup_path(local_config, argv[i]);
					doc_obj = ucl_object_lookup_path(cfg->doc_strings, argv[i]);
					if (!obj) {
						rspamd_printf("Local configuration for section %s NOT FOUND\n", argv[i]);
					}

					else {
						LL_FOREACH(obj, cur)
						{
							if (!json && !compact) {
								rspamd_printf("*** Section %s (local only) ***\n", argv[i]);
							}
							rspamadm_dump_section_obj(cfg, cur, doc_obj);
                            
							if (!json && !compact) {
								rspamd_printf("\n*** End of section %s (local only) ***\n", argv[i]);
							}
							else {
								rspamd_printf("\n");
							}
						}
					}
				}
				ucl_object_unref(local_config);
			}
		}

		else {
		
		// Original behavior for non-local-only configuration elements


		if (argc == 1) {
			rspamadm_dump_section_obj(cfg, cfg->cfg_ucl_obj, cfg->doc_strings);
		}
		else {
			for (i = 1; i < argc; i++) {
				obj = ucl_object_lookup_path(cfg->cfg_ucl_obj, argv[i]);
				doc_obj = ucl_object_lookup_path(cfg->doc_strings, argv[i]);

				if (!obj) {
					rspamd_printf("Section %s NOT FOUND\n", argv[i]);
				}
				else {
					LL_FOREACH(obj, cur)
					{
						if (!json && !compact) {
							rspamd_printf("*** Section %s ***\n", argv[i]);
						}
						rspamadm_dump_section_obj(cfg, cur, doc_obj);

						if (!json && !compact) {
							rspamd_printf("\n*** End of section %s ***\n", argv[i]);
						}
						else {
							rspamd_printf("\n");
						}
					}
				}
			}
		}
	}
	}

	exit(ret ? EXIT_SUCCESS : EXIT_FAILURE);
}
