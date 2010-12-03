/*
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * In addition to the permissions in the GNU General Public License,
 * the authors give you unlimited permission to link the compiled
 * version of this file into combinations with other programs,
 * and to distribute those combinations without any restriction
 * coming from the use of this file.  (The General Public License
 * restrictions do apply in other respects; for example, they cover
 * modification of the file, and distribution when not linked into
 * a combined executable.)
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#include <stdarg.h>

#include "common.h"
#include "repository.h"
#include "commit.h"
#include "tag.h"
#include "blob.h"
#include "fileops.h"

#define GIT_FOLDER "/.git/"
#define GIT_OBJECTS_FOLDER "objects/"
#define GIT_INDEX_FILE "index"
#define GIT_HEAD_FILE "HEAD"

static const int default_table_size = 32;
static const double max_load_factor = 0.65;

static const int OBJECT_BASE_SIZE = 4096;

static const size_t object_sizes[] = {
	0,
	sizeof(git_commit),
	sizeof(git_tree),
	sizeof(git_blob),
	sizeof(git_tag)
};


uint32_t git__objtable_hash(const void *key)
{
	uint32_t r;
	git_oid *id;

	id = (git_oid *)key;
	memcpy(&r, id->id, sizeof(r));
	return r;
}

int git__objtable_haskey(void *object, const void *key)
{
	git_object *obj;
	git_oid *oid;

	obj = (git_object *)object;
	oid = (git_oid *)key;

	return (git_oid_cmp(oid, &obj->id) == 0);
}



static int assign_repository_folders(git_repository *repo,
		const char *git_dir,
		const char *git_object_directory,
		const char *git_index_file,
		const char *git_work_tree)
{
	char path_aux[GIT_PATH_MAX];
	size_t path_len;

	assert(repo);

	if (git_dir == NULL || gitfo_isdir(git_dir) < 0)
		return GIT_ENOTFOUND;


	/* store GIT_DIR */
	path_len = strlen(git_dir);
	strcpy(path_aux, git_dir);

	if (path_aux[path_len - 1] != '/') {
		path_aux[path_len] = '/';
		path_aux[path_len + 1] = 0;

		path_len = path_len + 1;
	}

	repo->path_repository = git__strdup(path_aux);

	/* store GIT_OBJECT_DIRECTORY */
	if (git_object_directory == NULL)
		strcpy(path_aux + path_len, GIT_OBJECTS_FOLDER);
	else
		strcpy(path_aux, git_object_directory);

	if (gitfo_isdir(path_aux) < 0)
		return GIT_ENOTFOUND;

	repo->path_odb = git__strdup(path_aux);


	/* store GIT_INDEX_FILE */
	if (git_index_file == NULL)
		strcpy(path_aux + path_len, GIT_INDEX_FILE);
	else
		strcpy(path_aux, git_index_file); 

	if (gitfo_exists(path_aux) < 0)
		return GIT_ENOTFOUND;

	repo->path_index = git__strdup(path_aux);


	/* store GIT_WORK_TREE */
	if (git_work_tree == NULL)
		repo->is_bare = 1;
	else
		repo->path_workdir = git__strdup(git_work_tree);

	return GIT_SUCCESS;
}

static int guess_repository_folders(git_repository *repo, const char *repository_path)
{
	char path_aux[GIT_PATH_MAX], *last_folder;
	int path_len;

	if (gitfo_isdir(repository_path) < 0)
		return GIT_ENOTAREPO;

	path_len = strlen(repository_path);
	strcpy(path_aux, repository_path);

	if (path_aux[path_len - 1] != '/') {
		path_aux[path_len] = '/';
		path_aux[path_len + 1] = 0;

		path_len = path_len + 1;
	}

	repo->path_repository = git__strdup(path_aux);

	/* objects database */
	strcpy(path_aux + path_len, GIT_OBJECTS_FOLDER);
	if (gitfo_isdir(path_aux) < 0)
		return GIT_ENOTAREPO;
	repo->path_odb = git__strdup(path_aux);

	/* HEAD file */
	strcpy(path_aux + path_len, GIT_HEAD_FILE);
	if (gitfo_exists(path_aux) < 0)
		return GIT_ENOTAREPO;

	path_aux[path_len] = 0;

	last_folder = (path_aux + path_len - 2);

	while (*last_folder != '/')
		last_folder--;

	if (strcmp(last_folder, GIT_FOLDER) == 0) {
		repo->is_bare = 0;

		/* index file */
		strcpy(path_aux + path_len, GIT_INDEX_FILE);
		repo->path_index = git__strdup(path_aux);

		/* working dir */
		*(last_folder + 1) = 0;
		repo->path_workdir = git__strdup(path_aux);

	} else {
		repo->is_bare = 1;
		repo->path_workdir = NULL;
	}

	return GIT_SUCCESS;
}

git_repository *git_repository__alloc()
{
	git_repository *repo = git__malloc(sizeof(git_repository));
	if (!repo)
		return NULL;

	memset(repo, 0x0, sizeof(git_repository));

	repo->objects = git_hashtable_alloc(
			default_table_size, 
			git__objtable_hash,
			git__objtable_haskey);

	if (repo->objects == NULL) {
		free(repo);
		return NULL;
	}

	return repo;
}

int git_repository_open2(git_repository **repo_out,
		const char *git_dir,
		const char *git_object_directory,
		const char *git_index_file,
		const char *git_work_tree)
{
	git_repository *repo;
	int error = GIT_SUCCESS;

	assert(repo_out);

	repo = git_repository__alloc();
	if (repo == NULL)
		return GIT_ENOMEM;

	error = assign_repository_folders(repo, 
			git_dir, 
			git_object_directory,
			git_index_file,
			git_work_tree);

	if (error < 0)
		goto cleanup;

	error = git_odb_open(&repo->db, repo->path_odb);
	if (error < 0)
		goto cleanup;

	*repo_out = repo;
	return GIT_SUCCESS;

cleanup:
	git_repository_free(repo);
	return error;
}

int git_repository_open(git_repository **repo_out, const char *path)
{
	git_repository *repo;
	int error = GIT_SUCCESS;

	assert(repo_out && path);

	repo = git_repository__alloc();
	if (repo == NULL)
		return GIT_ENOMEM;

	error = guess_repository_folders(repo, path);
	if (error < 0)
		goto cleanup;

	error = git_odb_open(&repo->db, repo->path_odb);
	if (error < 0)
		goto cleanup;

	*repo_out = repo;
	return GIT_SUCCESS;

cleanup:
	git_repository_free(repo);
	return error;
}

void git_repository_free(git_repository *repo)
{
	git_hashtable_iterator it;
	git_object *object;

	assert(repo);

	free(repo->path_workdir);
	free(repo->path_index);
	free(repo->path_repository);
	free(repo->path_odb);

	git_hashtable_iterator_init(repo->objects, &it);

	while ((object = (git_object *)
				git_hashtable_iterator_next(&it)) != NULL)
		git_object_free(object);

	git_hashtable_free(repo->objects);
	git_odb_close(repo->db);
	git_index_free(repo->index);
	free(repo);
}

git_index *git_repository_index(git_repository *repo)
{
	if (repo->index == NULL) {
		if (git_index_open_inrepo(&repo->index, repo) < 0)
			return NULL;

		assert(repo->index);
	}

	return repo->index;
}

static int source_resize(git_odb_source *src)
{
	size_t write_offset, new_size;
	void *new_data;

	write_offset = (size_t)((char *)src->write_ptr - (char *)src->raw.data);

	new_size = src->raw.len * 2;
	if ((new_data = git__malloc(new_size)) == NULL)
		return GIT_ENOMEM;

	memcpy(new_data, src->raw.data, src->written_bytes);
	free(src->raw.data);

	src->raw.data = new_data;
	src->raw.len = new_size;
	src->write_ptr = (char *)new_data + write_offset;

	return GIT_SUCCESS;
}

int git__source_printf(git_odb_source *source, const char *format, ...)
{
	va_list arglist;
	int len, did_resize = 0;

	assert(source->open && source->write_ptr);

	va_start(arglist, format);

	len = vsnprintf(source->write_ptr, source->raw.len - source->written_bytes, format, arglist);

	while (source->written_bytes + len >= source->raw.len) {
		if (source_resize(source) < 0)
			return GIT_ENOMEM;

		did_resize = 1;
	}

	if (did_resize)
		vsnprintf(source->write_ptr, source->raw.len - source->written_bytes, format, arglist);

	source->write_ptr = (char *)source->write_ptr + len;
	source->written_bytes += len;

	return GIT_SUCCESS;
}

int git__source_write(git_odb_source *source, const void *bytes, size_t len)
{
	assert(source);

	assert(source->open && source->write_ptr);

	while (source->written_bytes + len >= source->raw.len) {
		if (source_resize(source) < 0)
			return GIT_ENOMEM;
	}

	memcpy(source->write_ptr, bytes, len);
	source->write_ptr = (char *)source->write_ptr + len;
	source->written_bytes += len;

	return GIT_SUCCESS;
}

static void prepare_write(git_object *object)
{
	if (object->source.write_ptr != NULL || object->source.open)
		git_object__source_close(object);

	/* TODO: proper size calculation */
	object->source.raw.data = git__malloc(OBJECT_BASE_SIZE);
	object->source.raw.len = OBJECT_BASE_SIZE;

	object->source.write_ptr = object->source.raw.data;
	object->source.written_bytes = 0;

	object->source.open = 1;
}

static int write_back(git_object *object)
{
	int error;
	git_oid new_id;

	assert(object);

	assert(object->source.open);
	assert(object->modified);

	object->source.raw.len = object->source.written_bytes;

	if ((error = git_odb_write(&new_id, object->repo->db, &object->source.raw)) < 0)
		return error;

	if (!object->in_memory)
		git_hashtable_remove(object->repo->objects, &object->id);

	git_oid_cpy(&object->id, &new_id);
	git_hashtable_insert(object->repo->objects, &object->id, object);

	object->source.write_ptr = NULL;
	object->source.written_bytes = 0;

	object->modified = 0;
	object->in_memory = 0;

	git_object__source_close(object);
	return GIT_SUCCESS;
}

int git_object__source_open(git_object *object)
{
	int error;

	assert(object && !object->in_memory);

	if (object->source.open)
		git_object__source_close(object);

	error = git_odb_read(&object->source.raw, object->repo->db, &object->id);
	if (error < 0)
		return error;

	object->source.open = 1;
	return GIT_SUCCESS;
}

void git_object__source_close(git_object *object)
{
	assert(object);

	if (object->source.open) {
		git_rawobj_close(&object->source.raw);
		object->source.open = 0;
	}
}

int git_object_write(git_object *object)
{
	int error;
	git_odb_source *source;

	assert(object);

	if (object->modified == 0)
		return GIT_SUCCESS;

	prepare_write(object);
	source = &object->source;

	switch (source->raw.type) {
	case GIT_OBJ_COMMIT:
		error = git_commit__writeback((git_commit *)object, source);
		break;

	case GIT_OBJ_TREE:
		error = git_tree__writeback((git_tree *)object, source);
		break;

	case GIT_OBJ_TAG:
		error = git_tag__writeback((git_tag *)object, source);
		break;

	case GIT_OBJ_BLOB:
		error = git_blob__writeback((git_blob *)object, source);
		break;

	default:
		error = GIT_ERROR;
		break;
	}

	if (error < 0) {
		git_object__source_close(object);
		return error;
	}

	return write_back(object);
}

void git_object_free(git_object *object)
{
	assert(object);

	git_object__source_close(object);
	git_hashtable_remove(object->repo->objects, &object->id);

	switch (object->source.raw.type) {
	case GIT_OBJ_COMMIT:
		git_commit__free((git_commit *)object);
		break;

	case GIT_OBJ_TREE:
		git_tree__free((git_tree *)object);
		break;

	case GIT_OBJ_TAG:
		git_tag__free((git_tag *)object);
		break;

	case GIT_OBJ_BLOB:
		git_blob__free((git_blob *)object);
		break;

	default:
		free(object);
		break;
	}
}

git_odb *git_repository_database(git_repository *repo)
{
	assert(repo);
	return repo->db;
}

const git_oid *git_object_id(git_object *obj)
{
	assert(obj);

	if (obj->in_memory)
		return NULL;

	return &obj->id;
}

git_otype git_object_type(git_object *obj)
{
	assert(obj);
	return obj->source.raw.type;
}

git_repository *git_object_owner(git_object *obj)
{
	assert(obj);
	return obj->repo;
}

int git_repository_newobject(git_object **object_out, git_repository *repo, git_otype type)
{
	git_object *object = NULL;

	assert(object_out && repo);

	*object_out = NULL;

	switch (type) {
	case GIT_OBJ_COMMIT:
	case GIT_OBJ_TAG:
	case GIT_OBJ_TREE:
	case GIT_OBJ_BLOB:
		break;

	default:
		return GIT_EINVALIDTYPE;
	}

	object = git__malloc(object_sizes[type]);

	if (object == NULL)
		return GIT_ENOMEM;

	memset(object, 0x0, object_sizes[type]);
	object->repo = repo;
	object->in_memory = 1;
	object->modified = 1;

	object->source.raw.type = type;

	*object_out = object;
	return GIT_SUCCESS;
}

int git_repository_lookup(git_object **object_out, git_repository *repo, const git_oid *id, git_otype type)
{
	git_object *object = NULL;
	git_rawobj obj_file;
	int error = 0;

	assert(repo && object_out && id);

	object = git_hashtable_lookup(repo->objects, id);
	if (object != NULL) {
		*object_out = object;
		return GIT_SUCCESS;
	}

	error = git_odb_read(&obj_file, repo->db, id);
	if (error < 0)
		return error;

	if (type != GIT_OBJ_ANY && type != obj_file.type)
		return GIT_EINVALIDTYPE;

	type = obj_file.type;

	object = git__malloc(object_sizes[type]);

	if (object == NULL)
		return GIT_ENOMEM;

	memset(object, 0x0, object_sizes[type]);

	/* Initialize parent object */
	git_oid_cpy(&object->id, id);
	object->repo = repo;
	memcpy(&object->source.raw, &obj_file, sizeof(git_rawobj));
	object->source.open = 1;

	switch (type) {

	case GIT_OBJ_COMMIT:
		error = git_commit__parse((git_commit *)object);
		break;

	case GIT_OBJ_TREE:
		error = git_tree__parse((git_tree *)object);
		break;

	case GIT_OBJ_TAG:
		error = git_tag__parse((git_tag *)object);
		break;

	case GIT_OBJ_BLOB:
		error = git_blob__parse((git_blob *)object);
		break;

	default:
		break;
	}

	if (error < 0) {
		git_object_free(object);
		return error;
	}

	git_object__source_close(object);
	git_hashtable_insert(repo->objects, &object->id, object);

	*object_out = object;
	return GIT_SUCCESS;
}

#define GIT_NEWOBJECT_TEMPLATE(obj, tp) \
	int git_##obj##_new(git_##obj **o, git_repository *repo) {\
		return git_repository_newobject((git_object **)o, repo, GIT_OBJ_##tp); } \
	int git_##obj##_lookup(git_##obj **o, git_repository *repo, const git_oid *id) { \
		return git_repository_lookup((git_object **)o, repo, id, GIT_OBJ_##tp); }

GIT_NEWOBJECT_TEMPLATE(commit, COMMIT)
GIT_NEWOBJECT_TEMPLATE(tag, TAG)
GIT_NEWOBJECT_TEMPLATE(tree, TREE)
GIT_NEWOBJECT_TEMPLATE(blob, BLOB)


