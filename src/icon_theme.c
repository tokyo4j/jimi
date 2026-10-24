#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "core.h"
#include "window.h"

#include <glib.h>

// Finds icon files by the XDG icon theme and desktop entry specifications.
// The icon files of the themes are indexed by name at startup, since probing
// their hundreds of directories would block the compositor on every lookup.

// TODO: Look up the themes the configured theme inherits.
#define FALLBACK_ICON_THEME "hicolor"

// By preference when a directory has an icon in both formats.
// TODO: Look up XPM icons as well once they can be drawn.
static const char *const icon_extensions[] = {"png", "svg"};

// A directory of an icon theme with the range of the sizes of its icons in
// pixels. The sizes are multiplied by the scale of the directory, since
// icons are drawn at fractional scales.
struct icon_dir {
	char *path; // Relative to the roots of the theme.
	int min_size, max_size;
};

// The icon file at <roots[root]>/<dirs[dir].path>/<name>.<extension> of a
// theme.
struct icon_file {
	int dir;
	int root;
	int extension; // The index in icon_extensions.
};

struct icon_theme {
	const char *name;
	// The directories of the theme in the base directories by precedence,
	// which may not exist. The theme is indexed again when one of them is
	// created, removed, or modified, which package managers do by updating
	// icon-theme.cache in them after installing icons.
	char **roots;
	struct timespec *root_mtimes; // Zero if the root does not exist.
	struct icon_dir *dirs; // From index.theme in the first root with it.
	int nr_dirs;
	// Icon name -> GArray of struct icon_file, in the order of the
	// directories and then the roots.
	GHashTable *files;
};

// The XDG data directories by precedence. Free with g_strfreev().
static char **
data_dirs(void)
{
	GPtrArray *dirs = g_ptr_array_new();
	g_ptr_array_add(dirs, g_strdup(g_get_user_data_dir()));
	for (const char *const *dir = g_get_system_data_dirs(); *dir; dir++) {
		g_ptr_array_add(dirs, g_strdup(*dir));
	}
	g_ptr_array_add(dirs, NULL);
	return (char **)g_ptr_array_free(dirs, FALSE);
}

// Free with g_strfreev().
static char **
icon_base_dirs(void)
{
	GPtrArray *dirs = g_ptr_array_new();
	g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".icons", NULL));
	char **data = data_dirs();
	for (char **dir = data; *dir; dir++) {
		g_ptr_array_add(dirs, g_build_filename(*dir, "icons", NULL));
	}
	g_strfreev(data);
	g_ptr_array_add(dirs, g_strdup("/usr/share/pixmaps"));
	g_ptr_array_add(dirs, NULL);
	return (char **)g_ptr_array_free(dirs, FALSE);
}

// A copy of the path of the icon file in the directory, or NULL if it is too
// long.
static char *
icon_path(const char *dir, const char *name, int extension)
{
	char path[PATH_MAX];
	int length = snprintf(path, sizeof(path), "%s/%s.%s", dir, name,
		icon_extensions[extension]);
	if (length >= (int)sizeof(path)) {
		return NULL;
	}
	char *copy = strdup(path);
	assert(copy);
	return copy;
}

// A copy of the path of the icon file in the directory, or NULL.
static char *
find_icon_file(const char *dir, const char *name)
{
	for (size_t i = 0; i < ARRAY_LEN(icon_extensions); i++) {
		char *path = icon_path(dir, name, i);
		if (path && access(path, R_OK) == 0) {
			return path;
		}
		free(path);
	}
	return NULL;
}

// The modification time of the directory, or zero if it does not exist.
static struct timespec
dir_mtime(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return (struct timespec){0};
	}
	return st.st_mtim;
}

static int
key_int(GKeyFile *file, const char *group, const char *key, int fallback)
{
	GError *error = NULL;
	int value = g_key_file_get_integer(file, group, key, &error);
	if (error) {
		g_error_free(error);
		return fallback;
	}
	return value;
}

static struct icon_dir
load_dir(GKeyFile *index, const char *path)
{
	int scale = key_int(index, path, "Scale", 1);
	int size = key_int(index, path, "Size", 0);
	int min = size;
	int max = size;
	char *type = g_key_file_get_string(index, path, "Type", NULL);
	if (!type || strcmp(type, "Threshold") == 0) {
		int threshold = key_int(index, path, "Threshold", 2);
		min = size - threshold;
		max = size + threshold;
	} else if (strcmp(type, "Scalable") == 0) {
		min = key_int(index, path, "MinSize", size);
		max = key_int(index, path, "MaxSize", size);
	}
	g_free(type);
	return (struct icon_dir){
		.path = g_strdup(path),
		.min_size = min * scale,
		.max_size = max * scale,
	};
}

static void
theme_load_dirs(struct icon_theme *theme)
{
	GKeyFile *index = g_key_file_new();
	// Unlike desktop entries, the lists are separated by commas.
	g_key_file_set_list_separator(index, ',');
	bool loaded = false;
	for (char **root = theme->roots; !loaded && *root; root++) {
		char *path = g_build_filename(*root, "index.theme", NULL);
		loaded = g_key_file_load_from_file(index, path, G_KEY_FILE_NONE, NULL);
		g_free(path);
	}
	char **paths = loaded ? g_key_file_get_string_list(index, "Icon Theme",
		"Directories", NULL, NULL) : NULL;
	theme->nr_dirs = paths ? g_strv_length(paths) : 0;
	theme->dirs = g_new(struct icon_dir, theme->nr_dirs);
	for (int i = 0; i < theme->nr_dirs; i++) {
		theme->dirs[i] = load_dir(index, paths[i]);
	}
	g_strfreev(paths);
	g_key_file_free(index);
}

static int
extension_index(const char *extension)
{
	for (size_t i = 0; i < ARRAY_LEN(icon_extensions); i++) {
		if (strcmp(extension, icon_extensions[i]) == 0) {
			return i;
		}
	}
	return -1;
}

static void
theme_scan_dir(struct icon_theme *theme, int dir, int root)
{
	char *path = g_build_filename(theme->roots[root], theme->dirs[dir].path, NULL);
	DIR *stream = opendir(path);
	g_free(path);
	if (!stream) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(stream))) {
		char *dot = strrchr(entry->d_name, '.');
		if (!dot || dot == entry->d_name) {
			continue;
		}
		int extension = extension_index(dot + 1);
		if (extension < 0) {
			continue;
		}
		char *name = g_strndup(entry->d_name, dot - entry->d_name);
		GArray *files = g_hash_table_lookup(theme->files, name);
		if (!files) {
			files = g_array_new(FALSE, FALSE, sizeof(struct icon_file));
			g_hash_table_insert(theme->files, name, files);
		} else {
			g_free(name);
		}
		// The icon may have been listed in the other format.
		struct icon_file *last = files->len > 0 ? &g_array_index(files,
			struct icon_file, files->len - 1) : NULL;
		if (last && last->dir == dir && last->root == root) {
			last->extension = MIN(last->extension, extension);
			continue;
		}
		struct icon_file file = {
			.dir = dir,
			.root = root,
			.extension = extension,
		};
		g_array_append_val(files, file);
	}
	closedir(stream);
}

static void
theme_load(struct icon_theme *theme, const char *name)
{
	theme->name = name;
	int nr_roots = g_strv_length(core.icon_themes.base_dirs);
	theme->roots = g_new0(char *, nr_roots + 1);
	theme->root_mtimes = g_new(struct timespec, nr_roots);
	for (int i = 0; i < nr_roots; i++) {
		theme->roots[i] = g_build_filename(core.icon_themes.base_dirs[i],
			name, NULL);
		theme->root_mtimes[i] = dir_mtime(theme->roots[i]);
	}
	theme_load_dirs(theme);
	theme->files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_array_unref);
	for (int dir = 0; dir < theme->nr_dirs; dir++) {
		for (int root = 0; root < nr_roots; root++) {
			// Skip the roots which do not exist.
			if (theme->root_mtimes[root].tv_sec != 0) {
				theme_scan_dir(theme, dir, root);
			}
		}
	}
}

static void
theme_fini(struct icon_theme *theme)
{
	for (int i = 0; i < theme->nr_dirs; i++) {
		g_free(theme->dirs[i].path);
	}
	g_free(theme->dirs);
	g_strfreev(theme->roots);
	g_free(theme->root_mtimes);
	g_hash_table_destroy(theme->files);
}

static void
theme_refresh(struct icon_theme *theme)
{
	for (int i = 0; theme->roots[i]; i++) {
		struct timespec mtime = dir_mtime(theme->roots[i]);
		if (mtime.tv_sec != theme->root_mtimes[i].tv_sec
				|| mtime.tv_nsec != theme->root_mtimes[i].tv_nsec) {
			const char *name = theme->name;
			theme_fini(theme);
			theme_load(theme, name);
			return;
		}
	}
}

// The distance from the size in pixels to the sizes of the icons in the
// directory, which is 0 if they match.
static int
dir_size_distance(const struct icon_dir *dir, int size)
{
	if (size < dir->min_size) {
		return dir->min_size - size;
	}
	if (size > dir->max_size) {
		return size - dir->max_size;
	}
	return 0;
}

// The icon of the size, or the closest one, in the theme. Among the closest
// ones, the first one in the order of the directories and then the roots is
// chosen.
static char *
theme_lookup(struct icon_theme *theme, const char *name, int size)
{
	GArray *files = g_hash_table_lookup(theme->files, name);
	if (!files) {
		return NULL;
	}
	struct icon_file *closest = NULL;
	int min_distance = INT_MAX;
	for (guint i = 0; i < files->len && min_distance > 0; i++) {
		struct icon_file *file = &g_array_index(files, struct icon_file, i);
		int distance = dir_size_distance(&theme->dirs[file->dir], size);
		if (distance < min_distance) {
			closest = file;
			min_distance = distance;
		}
	}
	char *dir = g_build_filename(theme->roots[closest->root],
		theme->dirs[closest->dir].path, NULL);
	char *path = icon_path(dir, name, closest->extension);
	g_free(dir);
	return path;
}

void
icon_themes_init(void)
{
	core.icon_themes.base_dirs = icon_base_dirs();
	core.icon_themes.themes = calloc(2, sizeof(*core.icon_themes.themes));
	assert(core.icon_themes.themes);
	const char *theme = config.decorations.icon_theme;
	theme_load(&core.icon_themes.themes[core.icon_themes.nr_themes++], theme);
	if (strcmp(theme, FALLBACK_ICON_THEME) != 0) {
		theme_load(&core.icon_themes.themes[core.icon_themes.nr_themes++],
			FALLBACK_ICON_THEME);
	}
}

void
icon_themes_fini(void)
{
	for (int i = 0; i < core.icon_themes.nr_themes; i++) {
		theme_fini(&core.icon_themes.themes[i]);
	}
	free(core.icon_themes.themes);
	core.icon_themes.nr_themes = 0;
	g_strfreev(core.icon_themes.base_dirs);
}

// Finds the file of the icon for the size in pixels, in the configured icon
// theme, hicolor, or outside themes. Returns NULL if there is none.
char *
icon_lookup(const char *name, int size)
{
	char *path = NULL;
	for (int i = 0; !path && i < core.icon_themes.nr_themes; i++) {
		theme_refresh(&core.icon_themes.themes[i]);
		path = theme_lookup(&core.icon_themes.themes[i], name, size);
	}
	for (char **dir = core.icon_themes.base_dirs; !path && *dir; dir++) {
		path = find_icon_file(*dir, name);
	}
	return path;
}

// The Icon key of the first desktop entry of the ID in the data
// directories. Free with g_free().
static char *
desktop_entry_icon(const char *id)
{
	char *file = g_strconcat(id, ".desktop", NULL);
	char **dirs = data_dirs();
	char *icon = NULL;
	bool found = false;
	for (char **dir = dirs; !found && *dir; dir++) {
		char *path = g_build_filename(*dir, "applications", file, NULL);
		GKeyFile *entry = g_key_file_new();
		found = g_key_file_load_from_file(entry, path, G_KEY_FILE_NONE, NULL);
		if (found) {
			icon = g_key_file_get_string(entry, "Desktop Entry", "Icon", NULL);
		}
		g_key_file_free(entry);
		g_free(path);
	}
	g_strfreev(dirs);
	g_free(file);
	return icon;
}

// Finds the file of the icon for the size in pixels by the desktop entry of
// the app ID, which is its desktop file ID. X11 windows give their class
// instead, which often differs in case. Apps without a desktop entry often
// name their icon after the app ID. Returns NULL if there is none.
char *
app_icon_lookup(const char *app_id, int size)
{
	char *icon = desktop_entry_icon(app_id);
	if (!icon) {
		char *lower = g_ascii_strdown(app_id, -1);
		icon = desktop_entry_icon(lower);
		g_free(lower);
	}
	char *path;
	if (icon && g_path_is_absolute(icon)) {
		path = strdup(icon);
		assert(path);
	} else {
		path = icon_lookup(icon ? icon : app_id, size);
	}
	g_free(icon);
	return path;
}
