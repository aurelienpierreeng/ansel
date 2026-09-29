# Redesign of selection API

> **First checked 2026-09-29.** This file was mechanically checked against `8f4638a04e` on 2026-09-29 — every `file:line` citation resolved, every backticked symbol looked up in the tree, every OPEN/planned status claim tested, and every gate or baseline number it quotes compared with `tools/check_module_boundaries.sh` and `tools/include_baseline.txt`. **No per-claim semantic read was done**: a citation that resolves can still describe the wrong thing, so this is a floor, not a verification. **Two signals named here do not exist** and are flagged in place: `DT_SIGNAL_FILMROLLS_IMPORTED` and `DT_SIGNAL_FILMFOLDER_CHANGED`.

[In progress]

`src/common/collection.h` and `src/common/selection.h` have been properly defiled since 2019. The code is duplicated by the thumbtable (`image_to_act.h`) and such. The whole thing has more than one way of setting and getting params, which sucks. But it also mixes C code (using `GList`) and SQL requests in a way that is really messy.

## Description of the new API

### Collections

Declared in `src/common/collection.h`. It's the backbone of the lighttable and filmstrip, extracted from the library database.

```C
typedef struct dt_collection_t
{
  gchar *query, *query_no_group;
  gchar **where_ext;
  unsigned int count, count_no_group;
  unsigned int tagid;
  dt_collection_params_t params;
  dt_collection_params_t store;

  // Unpack the list of imgid to avoid querying SQL al the time
  GList *id_list;
} dt_collection_t;
```

The whole collection API uses a `dt_collection_t *collection` argument, but the whole software really only uses one collection, stored in the `darktable.collection` structure member and accessed globally from there. It seems a sane practice to keep the API able to deal with another collection in the future, but it's slightly annoying to call all functions with always the same argument.

The `gchar *query` and `gchar *query_no_group` contain the SQL commands, built from the config strings stored in `anselrc`. From there, we only care about the list of image ids extracted from the library DB that will be linked with thumbnails and mipmap cache.

So, basically, the collection is a list of `imgid` output from an SQL extraction of the library, using filtering and sorting parameters defined by users in the `src/libs/collect.c` and `src/libs/tools/filter.c` GUI modules.

Updating the SQL queries is done by the `dt_collection_update_query()` function, that is called only from GUI modules `src/libs/collect.c` and `src/libs/tools/filter.c` and reads only the `anselrc` params, meaning the GUI controls should commit the settings to `anselrc` before calling dt_collection_update_query()`.

The collection might filter images based on user-editable properties, like rating, color labels and metadata. On user edition events, the content of the collection may change and needs to be reloaded with `dt_collection_update()`. This function is connected to various signals like `DT_SIGNAL_FILMROLLS_CHANGED`, `DT_SIGNAL_IMAGE_IMPORT`, `DT_SIGNAL_TAG_CHANGED`. (Checked 2026-09-29: there is no `DT_SIGNAL_FILMROLLS_IMPORTED`; the film-roll signals in `src/control/signal.h` are `DT_SIGNAL_FILMROLLS_CHANGED` (:159) and `DT_SIGNAL_FILMROLLS_REMOVED` (:162).) When a GUI widget performs property edition that may affect the content of the collection, it needs to raise the relevant signal (that you may create for your needs). The function should not be called directly.

`dt_collection_update()` is called from `dt_collection_update_query()` as well. When it finishes, it raises the signal `DT_SIGNAL_COLLECTION_CHANGED` that should be captured by GUI events that need to update the list of thumbnails, in lighttable and filmstrip.

Since a collection is ultimately just an ordered list of `imgid`, in `dt_collection_update()` we also create the `GList *id_list` as a member of the `struct dt_collection_t`. From there, no further SQL messup is required in the GUI code, except to grab further data linked to an image from the database (and deal with ugly image groups… See below)

__Collections are a MODEL in the view-model-controller scheme, they should only deal with information and contain no Gtk/GUI code__.

## Groups

Groups allow to define "sibling" images that can be collapsed or expanded in GUI. They are referenced by a unique ID in database. When collapsing/expanding, the ID of the current group is remembered at the app level and stored into `darktable.gui->expanded_group_id`. To reset it, it is set to `-1`.

Groups are handled in a particularly ugly way, through non-uniform special cases, in many places in the code. The most probable reason is they were introduced as an afterthought, after the initial design of collections. They need a better API.

Creating groups and duplicates currently raises a film-roll signal because they need to be inserted in the table. (Checked 2026-09-29: this named `DT_SIGNAL_FILMFOLDER_CHANGED`, which does not exist and has no close relative — the raise to look for is `DT_SIGNAL_FILMROLLS_CHANGED`, or a `dt_collection_update_query()` call, which is what most of these paths do now.) However, that may not work for collections filtering tags or metadata.
