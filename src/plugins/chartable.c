/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/***MODULE:chartable
 * rspamd module that make marks based on symbol chains
 *
 * Allowed options:
 * - symbol (string): symbol to insert (default: 'R_BAD_CHARSET')
 * - threshold (double): value that would be used as threshold in expression characters_changed / total_characters
 *   (e.g. if threshold is 0.1 than charset change should occure more often than in 10 symbols), default: 0.1
 */

#include "config.h"
#include "libmime/message.h"
#include "rspamd.h"
#include "libstat/stat_api.h"
#include "libstat/tokenizers/tokenizers.h"

#include "unicode/utf8.h"
#include "unicode/uchar.h"

#define DEFAULT_SYMBOL "R_MIXED_CHARSET"
#define DEFAULT_URL_SYMBOL "R_MIXED_CHARSET_URL"
#define DEFAULT_THRESHOLD 0.1

#define msg_err_chartable(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        "chartable", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_chartable(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        "chartable", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_chartable(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        "chartable", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)

#define msg_debug_chartable(...)  rspamd_conditional_debug_fast (NULL, task->from_addr, \
        rspamd_chartable_log_id, "chartable", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)

INIT_LOG_MODULE(chartable)

/* Initialization */
gint chartable_module_init (struct rspamd_config *cfg, struct module_ctx **ctx);
gint chartable_module_config (struct rspamd_config *cfg);
gint chartable_module_reconfig (struct rspamd_config *cfg);

module_t chartable_module = {
		"chartable",
		chartable_module_init,
		chartable_module_config,
		chartable_module_reconfig,
		NULL,
		RSPAMD_MODULE_VER,
		(guint)-1,
};

struct chartable_ctx {
	struct module_ctx ctx;
	const gchar *symbol;
	const gchar *url_symbol;
	double threshold;
	guint max_word_len;
};

static inline struct chartable_ctx *
chartable_get_context (struct rspamd_config *cfg)
{
	return (struct chartable_ctx *)g_ptr_array_index (cfg->c_modules,
			chartable_module.ctx_offset);
}

static void chartable_symbol_callback (struct rspamd_task *task, void *unused);
static void chartable_url_symbol_callback (struct rspamd_task *task, void *unused);

gint
chartable_module_init (struct rspamd_config *cfg, struct module_ctx **ctx)
{
	struct chartable_ctx *chartable_module_ctx;

	chartable_module_ctx = rspamd_mempool_alloc0 (cfg->cfg_pool,
			sizeof (*chartable_module_ctx));
	chartable_module_ctx->max_word_len = 10;

	*ctx = (struct module_ctx *)chartable_module_ctx;

	return 0;
}


gint
chartable_module_config (struct rspamd_config *cfg)
{
	const ucl_object_t *value;
	gint res = TRUE;
	struct chartable_ctx *chartable_module_ctx = chartable_get_context (cfg);

	if (!rspamd_config_is_module_enabled (cfg, "chartable")) {
		return TRUE;
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "chartable", "symbol")) != NULL) {
		chartable_module_ctx->symbol = ucl_obj_tostring (value);
	}
	else {
		chartable_module_ctx->symbol = DEFAULT_SYMBOL;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "chartable", "url_symbol")) != NULL) {
		chartable_module_ctx->url_symbol = ucl_obj_tostring (value);
	}
	else {
		chartable_module_ctx->url_symbol = DEFAULT_URL_SYMBOL;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "chartable", "threshold")) != NULL) {
		if (!ucl_obj_todouble_safe (value, &chartable_module_ctx->threshold)) {
			msg_warn_config ("invalid numeric value");
			chartable_module_ctx->threshold = DEFAULT_THRESHOLD;
		}
	}
	else {
		chartable_module_ctx->threshold = DEFAULT_THRESHOLD;
	}
	if ((value =
			rspamd_config_get_module_opt (cfg, "chartable", "max_word_len")) != NULL) {
		chartable_module_ctx->max_word_len = ucl_object_toint (value);
	}
	else {
		chartable_module_ctx->threshold = DEFAULT_THRESHOLD;
	}

	rspamd_symbols_cache_add_symbol (cfg->cache,
			chartable_module_ctx->symbol,
			0,
			chartable_symbol_callback,
			NULL,
			SYMBOL_TYPE_NORMAL,
			-1);
	rspamd_symbols_cache_add_symbol (cfg->cache,
			chartable_module_ctx->url_symbol,
			0,
			chartable_url_symbol_callback,
			NULL,
			SYMBOL_TYPE_NORMAL,
			-1);

	msg_info_config ("init internal chartable module");

	return res;
}

gint
chartable_module_reconfig (struct rspamd_config *cfg)
{
	return chartable_module_config (cfg);
}

static gint latin_confusable[] = {
	0x02028, 0x02029, 0x01680, 0x02000, 0x02001, 0x02002, 0x02003, 0x02004, 0x02005, 0x02006,
	0x02008, 0x02009, 0x0200a, 0x0205f, 0x000a0, 0x02007, 0x0202f, 0x007fa, 0x0fe4d, 0x0fe4e,
	0x0fe4f, 0x02010, 0x02011, 0x02012, 0x02013, 0x0fe58, 0x006d4, 0x02043, 0x002d7, 0x02212,
	0x02796, 0x02cba, 0x0060d, 0x0066b, 0x0201a, 0x000b8, 0x0a4f9, 0x0037e, 0x00903, 0x00a83,
	0x0ff1a, 0x00589, 0x00703, 0x00704, 0x016ec, 0x0fe30, 0x01803, 0x01809, 0x0205a, 0x005c3,
	0x002f8, 0x0a789, 0x02236, 0x002d0, 0x0a4fd, 0x0ff01, 0x001c3, 0x02d51, 0x00294, 0x00241,
	0x0097d, 0x013ae, 0x0a6eb, 0x1d16d, 0x02024, 0x00701, 0x00702, 0x0a60e, 0x10a50, 0x00660,
	0x006f0, 0x0a4f8, 0x0055d, 0x0ff07, 0x02018, 0x02019, 0x0201b, 0x02032, 0x02035, 0x0055a,
	0x005f3, 0x00060, 0x01fef, 0x0ff40, 0x000b4, 0x00384, 0x01ffd, 0x01fbd, 0x01fbf, 0x01ffe,
	0x002b9, 0x00374, 0x002c8, 0x002ca, 0x002cb, 0x002f4, 0x002bb, 0x002bd, 0x002bc, 0x002be,
	0x0a78c, 0x005d9, 0x007f4, 0x007f5, 0x0144a, 0x016cc, 0x16f51, 0x16f52, 0x0ff3b, 0x02768,
	0x02772, 0x03014, 0x0fd3e, 0x0ff3d, 0x02769, 0x02773, 0x03015, 0x0fd3f, 0x02774, 0x1d114,
	0x02775, 0x0204e, 0x0066d, 0x02217, 0x1031f, 0x01735, 0x02041, 0x02215, 0x02044, 0x02571,
	0x027cb, 0x029f8, 0x1d23a, 0x031d3, 0x03033, 0x02cc6, 0x030ce, 0x04e3f, 0x02f03, 0x0ff3c,
	0x0fe68, 0x02216, 0x027cd, 0x029f5, 0x029f9, 0x1d20f, 0x1d23b, 0x031d4, 0x04e36, 0x02f02,
	0x0a778, 0x002c4, 0x002c6, 0x016ed, 0x02795, 0x1029b, 0x02039, 0x0276e, 0x002c2, 0x1d236,
	0x01438, 0x016b2, 0x01400, 0x02e40, 0x030a0, 0x0a4ff, 0x0203a, 0x0276f, 0x002c3, 0x1d237,
	0x01433, 0x16f3f, 0x02053, 0x002dc, 0x01fc0, 0x0223c, 0x1d7d0, 0x1d7da, 0x1d7e4, 0x1d7ee,
	0x1d7f8, 0x0a75a, 0x001a7, 0x003e8, 0x0a644, 0x014bf, 0x0a6ef, 0x1d206, 0x1d7d1, 0x1d7db,
	0x1d7e5, 0x1d7ef, 0x1d7f9, 0x0a7ab, 0x0021c, 0x001b7, 0x0a76a, 0x02ccc, 0x00417, 0x004e0,
	0x16f3b, 0x118ca, 0x1d7d2, 0x1d7dc, 0x1d7e6, 0x1d7f0, 0x1d7fa, 0x013ce, 0x118af, 0x1d7d3,
	0x1d7dd, 0x1d7e7, 0x1d7f1, 0x1d7fb, 0x001bc, 0x118bb, 0x1d7d4, 0x1d7de, 0x1d7e8, 0x1d7f2,
	0x1d7fc, 0x02cd2, 0x00431, 0x013ee, 0x118d5, 0x1d212, 0x1d7d5, 0x1d7df, 0x1d7e9, 0x1d7f3,
	0x1d7fd, 0x104d2, 0x118c6, 0x00b03, 0x009ea, 0x00a6a, 0x1e8cb, 0x1d7d6, 0x1d7e0, 0x1d7ea,
	0x1d7f4, 0x1d7fe, 0x00223, 0x00222, 0x1031a, 0x00a67, 0x00b68, 0x009ed, 0x00d6d, 0x1d7d7,
	0x1d7e1, 0x1d7eb, 0x1d7f5, 0x1d7ff, 0x0a76e, 0x02cca, 0x118cc, 0x118ac, 0x118d6, 0x0237a,
	0x0ff41, 0x1d41a, 0x1d44e, 0x1d482, 0x1d4b6, 0x1d4ea, 0x1d51e, 0x1d552, 0x1d586, 0x1d5ba,
	0x1d5ee, 0x1d622, 0x1d656, 0x1d68a, 0x00251, 0x003b1, 0x1d6c2, 0x1d6fc, 0x1d736, 0x1d770,
	0x1d7aa, 0x00430, 0x0ff21, 0x1d400, 0x1d434, 0x1d468, 0x1d49c, 0x1d4d0, 0x1d504, 0x1d538,
	0x1d56c, 0x1d5a0, 0x1d5d4, 0x1d608, 0x1d63c, 0x1d670, 0x00391, 0x1d6a8, 0x1d6e2, 0x1d71c,
	0x1d756, 0x1d790, 0x00410, 0x013aa, 0x015c5, 0x0a4ee, 0x16f40, 0x102a0, 0x1d41b, 0x1d44f,
	0x1d483, 0x1d4b7, 0x1d4eb, 0x1d51f, 0x1d553, 0x1d587, 0x1d5bb, 0x1d5ef, 0x1d623, 0x1d657,
	0x1d68b, 0x00184, 0x0042c, 0x013cf, 0x015af, 0x0ff22, 0x0212c, 0x1d401, 0x1d435, 0x1d469,
	0x1d4d1, 0x1d505, 0x1d539, 0x1d56d, 0x1d5a1, 0x1d5d5, 0x1d609, 0x1d63d, 0x1d671, 0x0a7b4,
	0x00392, 0x1d6a9, 0x1d6e3, 0x1d71d, 0x1d757, 0x1d791, 0x00412, 0x013f4, 0x015f7, 0x0a4d0,
	0x10282, 0x102a1, 0x10301, 0x0ff43, 0x0217d, 0x1d41c, 0x1d450, 0x1d484, 0x1d4b8, 0x1d4ec,
	0x1d520, 0x1d554, 0x1d588, 0x1d5bc, 0x1d5f0, 0x1d624, 0x1d658, 0x1d68c, 0x01d04, 0x003f2,
	0x02ca5, 0x00441, 0x0abaf, 0x1043d, 0x1f74c, 0x118f2, 0x118e9, 0x0ff23, 0x0216d, 0x02102,
	0x0212d, 0x1d402, 0x1d436, 0x1d46a, 0x1d49e, 0x1d4d2, 0x1d56e, 0x1d5a2, 0x1d5d6, 0x1d60a,
	0x1d63e, 0x1d672, 0x003f9, 0x02ca4, 0x00421, 0x013df, 0x0a4da, 0x102a2, 0x10302, 0x10415,
	0x1051c, 0x0217e, 0x02146, 0x1d41d, 0x1d451, 0x1d485, 0x1d4b9, 0x1d4ed, 0x1d521, 0x1d555,
	0x1d589, 0x1d5bd, 0x1d5f1, 0x1d625, 0x1d659, 0x1d68d, 0x00501, 0x013e7, 0x0146f, 0x0a4d2,
	0x0216e, 0x02145, 0x1d403, 0x1d437, 0x1d46b, 0x1d49f, 0x1d4d3, 0x1d507, 0x1d53b, 0x1d56f,
	0x1d5a3, 0x1d5d7, 0x1d60b, 0x1d63f, 0x1d673, 0x013a0, 0x015de, 0x015ea, 0x0a4d3, 0x0212e,
	0x0ff45, 0x0212f, 0x02147, 0x1d41e, 0x1d452, 0x1d486, 0x1d4ee, 0x1d522, 0x1d556, 0x1d58a,
	0x1d5be, 0x1d5f2, 0x1d626, 0x1d65a, 0x1d68e, 0x0ab32, 0x00435, 0x004bd, 0x022ff, 0x0ff25,
	0x02130, 0x1d404, 0x1d438, 0x1d46c, 0x1d4d4, 0x1d508, 0x1d53c, 0x1d570, 0x1d5a4, 0x1d5d8,
	0x1d60c, 0x1d640, 0x1d674, 0x00395, 0x1d6ac, 0x1d6e6, 0x1d720, 0x1d75a, 0x1d794, 0x00415,
	0x02d39, 0x013ac, 0x0a4f0, 0x118a6, 0x118ae, 0x10286, 0x1d41f, 0x1d453, 0x1d487, 0x1d4bb,
	0x1d4ef, 0x1d523, 0x1d557, 0x1d58b, 0x1d5bf, 0x1d5f3, 0x1d627, 0x1d65b, 0x1d68f, 0x0ab35,
	0x0a799, 0x0017f, 0x01e9d, 0x00584, 0x1d213, 0x02131, 0x1d405, 0x1d439, 0x1d46d, 0x1d4d5,
	0x1d509, 0x1d53d, 0x1d571, 0x1d5a5, 0x1d5d9, 0x1d60d, 0x1d641, 0x1d675, 0x0a798, 0x003dc,
	0x1d7ca, 0x015b4, 0x0a4dd, 0x118c2, 0x118a2, 0x10287, 0x102a5, 0x10525, 0x0ff47, 0x0210a,
	0x1d420, 0x1d454, 0x1d488, 0x1d4f0, 0x1d524, 0x1d558, 0x1d58c, 0x1d5c0, 0x1d5f4, 0x1d628,
	0x1d65c, 0x1d690, 0x00261, 0x01d83, 0x0018d, 0x00581, 0x1d406, 0x1d43a, 0x1d46e, 0x1d4a2,
	0x1d4d6, 0x1d50a, 0x1d53e, 0x1d572, 0x1d5a6, 0x1d5da, 0x1d60e, 0x1d642, 0x1d676, 0x0050c,
	0x013c0, 0x013f3, 0x0a4d6, 0x0ff48, 0x0210e, 0x1d421, 0x1d489, 0x1d4bd, 0x1d4f1, 0x1d525,
	0x1d559, 0x1d58d, 0x1d5c1, 0x1d5f5, 0x1d629, 0x1d65d, 0x1d691, 0x004bb, 0x00570, 0x013c2,
	0x0ff28, 0x0210b, 0x0210c, 0x0210d, 0x1d407, 0x1d43b, 0x1d46f, 0x1d4d7, 0x1d573, 0x1d5a7,
	0x1d5db, 0x1d60f, 0x1d643, 0x1d677, 0x00397, 0x1d6ae, 0x1d6e8, 0x1d722, 0x1d75c, 0x1d796,
	0x02c8e, 0x0041d, 0x013bb, 0x0157c, 0x0a4e7, 0x102cf, 0x002db, 0x02373, 0x0ff49, 0x02170,
	0x02139, 0x02148, 0x1d422, 0x1d456, 0x1d48a, 0x1d4be, 0x1d4f2, 0x1d526, 0x1d55a, 0x1d58e,
	0x1d5c2, 0x1d5f6, 0x1d62a, 0x1d65e, 0x1d692, 0x00131, 0x1d6a4, 0x0026a, 0x00269, 0x003b9,
	0x01fbe, 0x0037a, 0x1d6ca, 0x1d704, 0x1d73e, 0x1d778, 0x1d7b2, 0x00456, 0x0a647, 0x004cf,
	0x0ab75, 0x013a5, 0x118c3, 0x0ff4a, 0x02149, 0x1d423, 0x1d457, 0x1d48b, 0x1d4bf, 0x1d4f3,
	0x1d527, 0x1d55b, 0x1d58f, 0x1d5c3, 0x1d5f7, 0x1d62b, 0x1d65f, 0x1d693, 0x003f3, 0x00458,
	0x0ff2a, 0x1d409, 0x1d43d, 0x1d471, 0x1d4a5, 0x1d4d9, 0x1d50d, 0x1d541, 0x1d575, 0x1d5a9,
	0x1d5dd, 0x1d611, 0x1d645, 0x1d679, 0x0a7b2, 0x0037f, 0x00408, 0x013ab, 0x0148d, 0x0a4d9,
	0x1d424, 0x1d458, 0x1d48c, 0x1d4c0, 0x1d4f4, 0x1d528, 0x1d55c, 0x1d590, 0x1d5c4, 0x1d5f8,
	0x1d62c, 0x1d660, 0x1d694, 0x0212a, 0x0ff2b, 0x1d40a, 0x1d43e, 0x1d472, 0x1d4a6, 0x1d4da,
	0x1d50e, 0x1d542, 0x1d576, 0x1d5aa, 0x1d5de, 0x1d612, 0x1d646, 0x1d67a, 0x0039a, 0x1d6b1,
	0x1d6eb, 0x1d725, 0x1d75f, 0x1d799, 0x02c94, 0x0041a, 0x013e6, 0x016d5, 0x0a4d7, 0x10518,
	0x005c0, 0x0007c, 0x02223, 0x023fd, 0x0ffe8, 0x00031, 0x00661, 0x006f1, 0x10320, 0x1e8c7,
	0x1d7cf, 0x1d7d9, 0x1d7e3, 0x1d7ed, 0x1d7f7, 0x00049, 0x0ff29, 0x02160, 0x02110, 0x02111,
	0x1d408, 0x1d43c, 0x1d470, 0x1d4d8, 0x1d540, 0x1d574, 0x1d5a8, 0x1d5dc, 0x1d610, 0x1d644,
	0x1d678, 0x00196, 0x0ff4c, 0x0217c, 0x02113, 0x1d425, 0x1d459, 0x1d48d, 0x1d4c1, 0x1d4f5,
	0x1d529, 0x1d55d, 0x1d591, 0x1d5c5, 0x1d5f9, 0x1d62d, 0x1d661, 0x1d695, 0x001c0, 0x00399,
	0x1d6b0, 0x1d6ea, 0x1d724, 0x1d75e, 0x1d798, 0x02c92, 0x00406, 0x004c0, 0x005d5, 0x005df,
	0x00627, 0x1ee00, 0x1ee80, 0x0fe8e, 0x0fe8d, 0x007ca, 0x02d4f, 0x016c1, 0x0a4f2, 0x16f28,
	0x1028a, 0x10309, 0x1d22a, 0x0216c, 0x02112, 0x1d40b, 0x1d43f, 0x1d473, 0x1d4db, 0x1d50f,
	0x1d543, 0x1d577, 0x1d5ab, 0x1d5df, 0x1d613, 0x1d647, 0x1d67b, 0x02cd0, 0x013de, 0x014aa,
	0x0a4e1, 0x16f16, 0x118a3, 0x118b2, 0x1041b, 0x10526, 0x0ff2d, 0x0216f, 0x02133, 0x1d40c,
	0x1d440, 0x1d474, 0x1d4dc, 0x1d510, 0x1d544, 0x1d578, 0x1d5ac, 0x1d5e0, 0x1d614, 0x1d648,
	0x1d67c, 0x0039c, 0x1d6b3, 0x1d6ed, 0x1d727, 0x1d761, 0x1d79b, 0x003fa, 0x02c98, 0x0041c,
	0x013b7, 0x015f0, 0x016d6, 0x0a4df, 0x102b0, 0x10311, 0x1d427, 0x1d45b, 0x1d48f, 0x1d4c3,
	0x1d4f7, 0x1d52b, 0x1d55f, 0x1d593, 0x1d5c7, 0x1d5fb, 0x1d62f, 0x1d663, 0x1d697, 0x00578,
	0x0057c, 0x0ff2e, 0x02115, 0x1d40d, 0x1d441, 0x1d475, 0x1d4a9, 0x1d4dd, 0x1d511, 0x1d579,
	0x1d5ad, 0x1d5e1, 0x1d615, 0x1d649, 0x1d67d, 0x0039d, 0x1d6b4, 0x1d6ee, 0x1d728, 0x1d762,
	0x1d79c, 0x02c9a, 0x0a4e0, 0x10513, 0x00c02, 0x00c82, 0x00d02, 0x00d82, 0x00966, 0x00a66,
	0x00ae6, 0x00be6, 0x00c66, 0x00ce6, 0x00d66, 0x00e50, 0x00ed0, 0x01040, 0x00665, 0x006f5,
	0x0ff4f, 0x02134, 0x1d428, 0x1d45c, 0x1d490, 0x1d4f8, 0x1d52c, 0x1d560, 0x1d594, 0x1d5c8,
	0x1d5fc, 0x1d630, 0x1d664, 0x1d698, 0x01d0f, 0x01d11, 0x0ab3d, 0x003bf, 0x1d6d0, 0x1d70a,
	0x1d744, 0x1d77e, 0x1d7b8, 0x003c3, 0x1d6d4, 0x1d70e, 0x1d748, 0x1d782, 0x1d7bc, 0x02c9f,
	0x0043e, 0x010ff, 0x00585, 0x005e1, 0x00647, 0x1ee24, 0x1ee64, 0x1ee84, 0x0feeb, 0x0feec,
	0x0feea, 0x0fee9, 0x006be, 0x0fbac, 0x0fbad, 0x0fbab, 0x0fbaa, 0x006c1, 0x0fba8, 0x0fba9,
	0x0fba7, 0x0fba6, 0x006d5, 0x00d20, 0x0101d, 0x104ea, 0x118c8, 0x118d7, 0x1042c, 0x00030,
	0x007c0, 0x009e6, 0x00b66, 0x03007, 0x114d0, 0x118e0, 0x1d7ce, 0x1d7d8, 0x1d7e2, 0x1d7ec,
	0x1d7f6, 0x0ff2f, 0x1d40e, 0x1d442, 0x1d476, 0x1d4aa, 0x1d4de, 0x1d512, 0x1d546, 0x1d57a,
	0x1d5ae, 0x1d5e2, 0x1d616, 0x1d64a, 0x1d67e, 0x0039f, 0x1d6b6, 0x1d6f0, 0x1d72a, 0x1d764,
	0x1d79e, 0x02c9e, 0x0041e, 0x00555, 0x02d54, 0x012d0, 0x00b20, 0x104c2, 0x0a4f3, 0x118b5,
	0x10292, 0x102ab, 0x10404, 0x10516, 0x02374, 0x0ff50, 0x1d429, 0x1d45d, 0x1d491, 0x1d4c5,
	0x1d4f9, 0x1d52d, 0x1d561, 0x1d595, 0x1d5c9, 0x1d5fd, 0x1d631, 0x1d665, 0x1d699, 0x003c1,
	0x003f1, 0x1d6d2, 0x1d6e0, 0x1d70c, 0x1d71a, 0x1d746, 0x1d754, 0x1d780, 0x1d78e, 0x1d7ba,
	0x1d7c8, 0x02ca3, 0x00440, 0x0ff30, 0x02119, 0x1d40f, 0x1d443, 0x1d477, 0x1d4ab, 0x1d4df,
	0x1d513, 0x1d57b, 0x1d5af, 0x1d5e3, 0x1d617, 0x1d64b, 0x1d67f, 0x003a1, 0x1d6b8, 0x1d6f2,
	0x1d72c, 0x1d766, 0x1d7a0, 0x02ca2, 0x00420, 0x013e2, 0x0146d, 0x0a4d1, 0x10295, 0x1d42a,
	0x1d45e, 0x1d492, 0x1d4c6, 0x1d4fa, 0x1d52e, 0x1d562, 0x1d596, 0x1d5ca, 0x1d5fe, 0x1d632,
	0x1d666, 0x1d69a, 0x0051b, 0x00563, 0x00566, 0x0211a, 0x1d410, 0x1d444, 0x1d478, 0x1d4ac,
	0x1d4e0, 0x1d514, 0x1d57c, 0x1d5b0, 0x1d5e4, 0x1d618, 0x1d64c, 0x1d680, 0x02d55, 0x1d42b,
	0x1d45f, 0x1d493, 0x1d4c7, 0x1d4fb, 0x1d52f, 0x1d563, 0x1d597, 0x1d5cb, 0x1d5ff, 0x1d633,
	0x1d667, 0x1d69b, 0x0ab47, 0x0ab48, 0x01d26, 0x02c85, 0x00433, 0x0ab81, 0x1d216, 0x0211b,
	0x0211c, 0x0211d, 0x1d411, 0x1d445, 0x1d479, 0x1d4e1, 0x1d57d, 0x1d5b1, 0x1d5e5, 0x1d619,
	0x1d64d, 0x1d681, 0x001a6, 0x013a1, 0x013d2, 0x104b4, 0x01587, 0x0a4e3, 0x16f35, 0x0ff53,
	0x1d42c, 0x1d460, 0x1d494, 0x1d4c8, 0x1d4fc, 0x1d530, 0x1d564, 0x1d598, 0x1d5cc, 0x1d600,
	0x1d634, 0x1d668, 0x1d69c, 0x0a731, 0x001bd, 0x00455, 0x0abaa, 0x118c1, 0x10448, 0x0ff33,
	0x1d412, 0x1d446, 0x1d47a, 0x1d4ae, 0x1d4e2, 0x1d516, 0x1d54a, 0x1d57e, 0x1d5b2, 0x1d5e6,
	0x1d61a, 0x1d64e, 0x1d682, 0x00405, 0x0054f, 0x013d5, 0x013da, 0x0a4e2, 0x16f3a, 0x10296,
	0x10420, 0x1d42d, 0x1d461, 0x1d495, 0x1d4c9, 0x1d4fd, 0x1d531, 0x1d565, 0x1d599, 0x1d5cd,
	0x1d601, 0x1d635, 0x1d669, 0x1d69d, 0x022a4, 0x027d9, 0x1f768, 0x0ff34, 0x1d413, 0x1d447,
	0x1d47b, 0x1d4af, 0x1d4e3, 0x1d517, 0x1d54b, 0x1d57f, 0x1d5b3, 0x1d5e7, 0x1d61b, 0x1d64f,
	0x1d683, 0x003a4, 0x1d6bb, 0x1d6f5, 0x1d72f, 0x1d769, 0x1d7a3, 0x02ca6, 0x00422, 0x013a2,
	0x0a4d4, 0x16f0a, 0x118bc, 0x10297, 0x102b1, 0x10315, 0x1d42e, 0x1d462, 0x1d496, 0x1d4ca,
	0x1d4fe, 0x1d532, 0x1d566, 0x1d59a, 0x1d5ce, 0x1d602, 0x1d636, 0x1d66a, 0x1d69e, 0x0a79f,
	0x01d1c, 0x0ab4e, 0x0ab52, 0x0028b, 0x003c5, 0x1d6d6, 0x1d710, 0x1d74a, 0x1d784, 0x1d7be,
	0x0057d, 0x104f6, 0x118d8, 0x0222a, 0x022c3, 0x1d414, 0x1d448, 0x1d47c, 0x1d4b0, 0x1d4e4,
	0x1d518, 0x1d54c, 0x1d580, 0x1d5b4, 0x1d5e8, 0x1d61c, 0x1d650, 0x1d684, 0x0054d, 0x01200,
	0x104ce, 0x0144c, 0x0a4f4, 0x16f42, 0x118b8, 0x02228, 0x022c1, 0x0ff56, 0x02174, 0x1d42f,
	0x1d463, 0x1d497, 0x1d4cb, 0x1d4ff, 0x1d533, 0x1d567, 0x1d59b, 0x1d5cf, 0x1d603, 0x1d637,
	0x1d66b, 0x1d69f, 0x01d20, 0x003bd, 0x1d6ce, 0x1d708, 0x1d742, 0x1d77c, 0x1d7b6, 0x00475,
	0x005d8, 0x11706, 0x0aba9, 0x118c0, 0x1d20d, 0x00667, 0x006f7, 0x02164, 0x1d415, 0x1d449,
	0x1d47d, 0x1d4b1, 0x1d4e5, 0x1d519, 0x1d54d, 0x1d581, 0x1d5b5, 0x1d5e9, 0x1d61d, 0x1d651,
	0x1d685, 0x00474, 0x02d38, 0x013d9, 0x0142f, 0x0a6df, 0x0a4e6, 0x16f08, 0x118a0, 0x1051d,
	0x0026f, 0x1d430, 0x1d464, 0x1d498, 0x1d4cc, 0x1d500, 0x1d534, 0x1d568, 0x1d59c, 0x1d5d0,
	0x1d604, 0x1d638, 0x1d66c, 0x1d6a0, 0x01d21, 0x00461, 0x0051d, 0x00561, 0x1170a, 0x1170e,
	0x1170f, 0x0ab83, 0x118ef, 0x118e6, 0x1d416, 0x1d44a, 0x1d47e, 0x1d4b2, 0x1d4e6, 0x1d51a,
	0x1d54e, 0x1d582, 0x1d5b6, 0x1d5ea, 0x1d61e, 0x1d652, 0x1d686, 0x0051c, 0x013b3, 0x013d4,
	0x0a4ea, 0x0166e, 0x000d7, 0x0292b, 0x0292c, 0x02a2f, 0x0ff58, 0x02179, 0x1d431, 0x1d465,
	0x1d499, 0x1d4cd, 0x1d501, 0x1d535, 0x1d569, 0x1d59d, 0x1d5d1, 0x1d605, 0x1d639, 0x1d66d,
	0x1d6a1, 0x00445, 0x01541, 0x0157d, 0x0166d, 0x02573, 0x10322, 0x118ec, 0x0ff38, 0x02169,
	0x1d417, 0x1d44b, 0x1d47f, 0x1d4b3, 0x1d4e7, 0x1d51b, 0x1d54f, 0x1d583, 0x1d5b7, 0x1d5eb,
	0x1d61f, 0x1d653, 0x1d687, 0x0a7b3, 0x003a7, 0x1d6be, 0x1d6f8, 0x1d732, 0x1d76c, 0x1d7a6,
	0x02cac, 0x00425, 0x02d5d, 0x016b7, 0x0a4eb, 0x10290, 0x102b4, 0x10317, 0x10527, 0x00263,
	0x01d8c, 0x0ff59, 0x1d432, 0x1d466, 0x1d49a, 0x1d4ce, 0x1d502, 0x1d536, 0x1d56a, 0x1d59e,
	0x1d5d2, 0x1d606, 0x1d63a, 0x1d66e, 0x1d6a2, 0x0028f, 0x01eff, 0x0ab5a, 0x003b3, 0x0213d,
	0x1d6c4, 0x1d6fe, 0x1d738, 0x1d772, 0x1d7ac, 0x00443, 0x004af, 0x010e7, 0x118dc, 0x0ff39,
	0x1d418, 0x1d44c, 0x1d480, 0x1d4b4, 0x1d4e8, 0x1d51c, 0x1d550, 0x1d584, 0x1d5b8, 0x1d5ec,
	0x1d620, 0x1d654, 0x1d688, 0x003a5, 0x003d2, 0x1d6bc, 0x1d6f6, 0x1d730, 0x1d76a, 0x1d7a4,
	0x02ca8, 0x00423, 0x004ae, 0x013a9, 0x013bd, 0x0a4ec, 0x16f43, 0x118a4, 0x102b2, 0x1d433,
	0x1d467, 0x1d49b, 0x1d4cf, 0x1d503, 0x1d537, 0x1d56b, 0x1d59f, 0x1d5d3, 0x1d607, 0x1d63b,
	0x1d66f, 0x1d6a3, 0x01d22, 0x0ab93, 0x118c4, 0x102f5, 0x118e5, 0x0ff3a, 0x02124, 0x02128,
	0x1d419, 0x1d44d, 0x1d481, 0x1d4b5, 0x1d4e9, 0x1d585, 0x1d5b9, 0x1d5ed, 0x1d621, 0x1d655,
	0x1d689, 0x00396, 0x1d6ad, 0x1d6e7, 0x1d721, 0x1d75b, 0x1d795, 0x013c3, 0x0a4dc, 0x118a9,
};

GHashTable *latin_confusable_ht = NULL;

static gboolean
rspamd_can_alias_latin (gint ch)
{
	if (latin_confusable_ht == NULL) {
		guint i;

		/* Build hash table */
		latin_confusable_ht = g_hash_table_new (g_int_hash, g_int_equal);

		for (i = 0; i < G_N_ELEMENTS (latin_confusable); i ++) {
			g_hash_table_insert(latin_confusable_ht, &latin_confusable[i],
					GINT_TO_POINTER (-1));
		}
	}

	return g_hash_table_lookup (latin_confusable_ht, &ch) != NULL;
}

static gdouble
rspamd_chartable_process_word_utf (struct rspamd_task *task,
								   rspamd_stat_token_t *w,
								   gboolean is_url,
								   guint *ncap,
								   struct chartable_ctx *chartable_module_ctx)
{
	const gchar *p, *end;
	gdouble badness = 0.0;
	UChar32 uc;
	UBlockCode sc;
	gint last_is_latin = -1;
	guint same_script_count = 0, nsym = 0, i = 0;
	enum {
		start_process = 0,
		got_alpha,
		got_digit,
		got_unknown,
	} state = start_process, prev_state = start_process;

	p = w->begin;
	end = p + w->len;

	/* We assume that w is normalized */

	while (p + i < end) {
		U8_NEXT (p, i, w->len, uc);

		if (((gint32)uc) < 0) {
			break;
		}

		if (u_isalpha (uc)) {
			sc = ublock_getCode (uc);
			if (sc <= UBLOCK_COMBINING_DIACRITICAL_MARKS ||
					sc == UBLOCK_LATIN_EXTENDED_ADDITIONAL) {
				/*
				 * Assume all latin, IPA, diacritic and space modifiers
				 * characters as basic latin
				 */
				sc = UBLOCK_BASIC_LATIN;
			}

			if (sc != UBLOCK_BASIC_LATIN && u_isupper (uc)) {
				if (ncap) {
					(*ncap) ++;
				}
			}

			if (state == got_digit) {
				/* Penalize digit -> alpha translations */
				if (!is_url && sc != UBLOCK_BASIC_LATIN &&
						prev_state != start_process) {
					badness += 0.25;
				}
			}
			else if (state == got_alpha) {
				/* Check script */
				if (same_script_count > 0) {
					if (sc != UBLOCK_BASIC_LATIN && last_is_latin) {

						if (rspamd_can_alias_latin (uc)) {
							badness += 1.0 / (gdouble)same_script_count;
						}

						last_is_latin = 0;
						same_script_count = 1;
					}
					else {
						same_script_count ++;
					}
				}
				else {
					last_is_latin = sc == UBLOCK_BASIC_LATIN;
					same_script_count = 1;
				}
			}

			prev_state = state;
			state = got_alpha;

		}
		else if (u_isdigit (uc)) {
			if (state != got_digit) {
				prev_state = state;
			}

			state = got_digit;
			same_script_count = 0;
		}
		else {
			/* We don't care about unknown characters here */
			if (state != got_unknown) {
				prev_state = state;
			}

			state = got_unknown;
			same_script_count = 0;
		}

		nsym ++;
	}

	/* Try to avoid FP for long words */
	if (nsym > chartable_module_ctx->max_word_len) {
		badness = 0;
	}
	else {
		if (badness > 4.0) {
			badness = 4.0;
		}
	}

	msg_debug_chartable ("word %*s, badness: %.2f", (gint)w->len, w->begin,
			badness);

	return badness;
}

static gdouble
rspamd_chartable_process_word_ascii (struct rspamd_task *task,
									 rspamd_stat_token_t *w,
									 gboolean is_url,
									 struct chartable_ctx *chartable_module_ctx)
{
	const guchar *p, *end;
	gdouble badness = 0.0;
	enum {
		ascii = 1,
		non_ascii
	} sc, last_sc;
	gint same_script_count = 0, seen_alpha = FALSE;
	enum {
		start_process = 0,
		got_alpha,
		got_digit,
		got_unknown,
	} state = start_process;

	p = w->begin;
	end = p + w->len;
	last_sc = 0;

	if (w->len > chartable_module_ctx->max_word_len) {
		return 0.0;
	}

	/* We assume that w is normalized */
	while (p < end) {
		if (g_ascii_isalpha (*p) || *p > 0x7f) {

			if (state == got_digit) {
				/* Penalize digit -> alpha translations */
				if (seen_alpha && !is_url && !g_ascii_isxdigit (*p)) {
					badness += 0.25;
				}
			}
			else if (state == got_alpha) {
				/* Check script */
				sc = (*p > 0x7f) ? ascii : non_ascii;

				if (same_script_count > 0) {
					if (sc != last_sc) {
						badness += 1.0 / (gdouble)same_script_count;
						last_sc = sc;
						same_script_count = 1;
					}
					else {
						same_script_count ++;
					}
				}
				else {
					last_sc = sc;
					same_script_count = 1;
				}
			}

			seen_alpha = TRUE;
			state = got_alpha;

		}
		else if (g_ascii_isdigit (*p)) {
			state = got_digit;
			same_script_count = 0;
		}
		else {
			/* We don't care about unknown characters here */
			state = got_unknown;
			same_script_count = 0;
		}

		p ++;
	}

	if (badness > 4.0) {
		badness = 4.0;
	}

	msg_debug_chartable ("word %*s, badness: %.2f", (gint)w->len, w->begin,
			badness);

	return badness;
}

static void
rspamd_chartable_process_part (struct rspamd_task *task,
							   struct rspamd_mime_text_part *part,
							   struct chartable_ctx *chartable_module_ctx)
{
	rspamd_stat_token_t *w;
	guint i, ncap = 0;
	gdouble cur_score = 0.0;

	if (part == NULL || part->normalized_words == NULL ||
			part->normalized_words->len == 0) {
		return;
	}

	for (i = 0; i < part->normalized_words->len; i++) {
		w = &g_array_index (part->normalized_words, rspamd_stat_token_t, i);

		if (w->len > 0 && (w->flags & RSPAMD_STAT_TOKEN_FLAG_TEXT)) {

			if (IS_PART_UTF (part)) {
				cur_score += rspamd_chartable_process_word_utf (task, w, FALSE,
						&ncap, chartable_module_ctx);
			}
			else {
				cur_score += rspamd_chartable_process_word_ascii (task, w,
						FALSE, chartable_module_ctx);
			}
		}
	}

	/*
	 * TODO: perhaps, we should do this analysis somewhere else and get
	 * something like: <SYM_SC><SYM_SC><SYM_SC> representing classes for all
	 * symbols in the text
	 */
	part->capital_letters += ncap;

	cur_score /= (gdouble)part->normalized_words->len;

	if (cur_score > 2.0) {
		cur_score = 2.0;
	}

	if (cur_score > chartable_module_ctx->threshold) {
		rspamd_task_insert_result (task, chartable_module_ctx->symbol,
				cur_score, NULL);

	}
}

static void
chartable_symbol_callback (struct rspamd_task *task, void *unused)
{
	guint i;
	struct rspamd_mime_text_part *part;
	struct chartable_ctx *chartable_module_ctx = chartable_get_context (task->cfg);

	for (i = 0; i < task->text_parts->len; i ++) {
		part = g_ptr_array_index (task->text_parts, i);
		rspamd_chartable_process_part (task, part, chartable_module_ctx);
	}

	if (task->subject != NULL) {
		GArray *words;
		rspamd_stat_token_t *w;
		guint i;
		gdouble cur_score = 0.0;

		words = rspamd_tokenize_text (task->subject, strlen (task->subject),
				RSPAMD_TOKENIZE_UTF,
				NULL,
				NULL,
				NULL);

		if (words && words->len > 0) {
			for (i = 0; i < words->len; i++) {
				w = &g_array_index (words, rspamd_stat_token_t, i);
				cur_score += rspamd_chartable_process_word_utf (task, w, FALSE,
						NULL, chartable_module_ctx);
			}

			cur_score /= (gdouble)words->len;

			if (cur_score > 2.0) {
				cur_score = 2.0;
			}

			if (cur_score > chartable_module_ctx->threshold) {
				rspamd_task_insert_result (task, chartable_module_ctx->symbol,
						cur_score, "subject");

			}
		}

		if (words) {
			g_array_free (words, TRUE);
		}
	}
}

static void
chartable_url_symbol_callback (struct rspamd_task *task, void *unused)
{
	struct rspamd_url *u;
	GHashTableIter it;
	gpointer k, v;
	rspamd_stat_token_t w;
	gdouble cur_score = 0.0;
	struct chartable_ctx *chartable_module_ctx = chartable_get_context (task->cfg);

	g_hash_table_iter_init (&it, task->urls);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		u = v;

		if (cur_score > 2.0) {
			cur_score = 2.0;
			break;
		}

		if (u->hostlen > 0) {
			w.begin = u->host;
			w.len = u->hostlen;

			if (g_utf8_validate (w.begin, w.len, NULL)) {
				cur_score += rspamd_chartable_process_word_utf (task, &w,
						TRUE, NULL, chartable_module_ctx);
			}
			else {
				cur_score += rspamd_chartable_process_word_ascii (task, &w,
						TRUE, chartable_module_ctx);
			}
		}
	}

	g_hash_table_iter_init (&it, task->emails);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		u = v;

		if (cur_score > 2.0) {
			cur_score = 2.0;
			break;
		}

		if (u->hostlen > 0) {
			w.begin = u->host;
			w.len = u->hostlen;

			if (g_utf8_validate (w.begin, w.len, NULL)) {
				cur_score += rspamd_chartable_process_word_utf (task, &w,
						TRUE, NULL, chartable_module_ctx);
			}
			else {
				cur_score += rspamd_chartable_process_word_ascii (task, &w,
						TRUE, chartable_module_ctx);
			}
		}
	}

	if (cur_score > chartable_module_ctx->threshold) {
		rspamd_task_insert_result (task, chartable_module_ctx->symbol,
				cur_score, NULL);

	}
}
