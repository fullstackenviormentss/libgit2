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

#include "common.h"
#include "git/zlib.h"
#include "fileops.h"
#include "hash.h"
#include "odb.h"
#include "delta-apply.h"

static struct {
	const char *str;   /* type name string */
	int        loose;  /* valid loose object type flag */
} obj_type_table[] = {
	{ "",          0 },  /* 0 = GIT_OBJ__EXT1     */
	{ "commit",    1 },  /* 1 = GIT_OBJ_COMMIT    */
	{ "tree",      1 },  /* 2 = GIT_OBJ_TREE      */
	{ "blob",      1 },  /* 3 = GIT_OBJ_BLOB      */
	{ "tag",       1 },  /* 4 = GIT_OBJ_TAG       */
	{ "",          0 },  /* 5 = GIT_OBJ__EXT2     */
	{ "OFS_DELTA", 0 },  /* 6 = GIT_OBJ_OFS_DELTA */
	{ "REF_DELTA", 0 }   /* 7 = GIT_OBJ_REF_DELTA */
};

/***********************************************************
 *
 * MISCELANEOUS HELPER FUNCTIONS
 * 
 ***********************************************************/

const char *git_otype_tostring(git_otype type)
{
	if (type < 0 || ((size_t) type) >= ARRAY_SIZE(obj_type_table))
		return "";
	return obj_type_table[type].str;
}

git_otype git_otype_fromstring(const char *str)
{
	size_t i;

	if (!str || !*str)
		return GIT_OBJ_BAD;

	for (i = 0; i < ARRAY_SIZE(obj_type_table); i++)
		if (!strcmp(str, obj_type_table[i].str))
			return (git_otype) i;

	return GIT_OBJ_BAD;
}

int git_otype_is_loose(git_otype type)
{
	if (type < 0 || ((size_t) type) >= ARRAY_SIZE(obj_type_table))
		return 0;
	return obj_type_table[type].loose;
}

static int format_object_header(char *hdr, size_t n, git_rawobj *obj)
{
	const char *type_str = git_otype_tostring(obj->type);
	int len = snprintf(hdr, n, "%s %"PRIuZ, type_str, obj->len);

	assert(len > 0);             /* otherwise snprintf() is broken  */
	assert(((size_t) len) < n);  /* otherwise the caller is broken! */

	if (len < 0 || ((size_t) len) >= n)
		return GIT_ERROR;
	return len+1;
}

int git_odb__hash_obj(git_oid *id, char *hdr, size_t n, int *len, git_rawobj *obj)
{
	git_buf_vec vec[2];
	int  hdrlen;

	assert(id && hdr && len && obj);

	if (!git_otype_is_loose(obj->type))
		return GIT_ERROR;

	if (!obj->data && obj->len != 0)
		return GIT_ERROR;

	if ((hdrlen = format_object_header(hdr, n, obj)) < 0)
		return GIT_ERROR;

	*len = hdrlen;

	vec[0].data = hdr;
	vec[0].len  = hdrlen;
	vec[1].data = obj->data;
	vec[1].len  = obj->len;

	git_hash_vec(id, vec, 2);

	return GIT_SUCCESS;
}

int git_rawobj_hash(git_oid *id, git_rawobj *obj)
{
	char hdr[64];
	int  hdrlen;

	assert(id && obj);

	return git_odb__hash_obj(id, hdr, sizeof(hdr), &hdrlen, obj);
}

int git_odb__inflate_buffer(void *in, size_t inlen, void *out, size_t outlen)
{
	z_stream zs;
	int status = Z_OK;

	memset(&zs, 0x0, sizeof(zs));

	zs.next_out  = out;
	zs.avail_out = outlen;

	zs.next_in  = in;
	zs.avail_in = inlen;

	if (inflateInit(&zs) < Z_OK)
		return GIT_ERROR;

	while (status == Z_OK)
		status = inflate(&zs, Z_FINISH);

	inflateEnd(&zs);

	if ((status != Z_STREAM_END) /*|| (zs.avail_in != 0) */)
		return GIT_ERROR;

	if (zs.total_out != outlen)
		return GIT_ERROR;

	return GIT_SUCCESS;
}





/***********************************************************
 *
 * OBJECT DATABASE PUBLIC API
 *
 * Public calls for the ODB functionality
 * 
 ***********************************************************/

int backend_sort_cmp(const void *a, const void *b)
{
	const git_odb_backend *backend_a = *(const git_odb_backend **)(a);
	const git_odb_backend *backend_b = *(const git_odb_backend **)(b);

	return (backend_b->priority - backend_a->priority);
}

int git_odb_new(git_odb **out)
{
	git_odb *db = git__calloc(1, sizeof(*db));
	if (!db)
		return GIT_ENOMEM;

	if (git_vector_init(&db->backends, 4, backend_sort_cmp, NULL) < 0) {
		free(db);
		return GIT_ENOMEM;
	}

	*out = db;
	return GIT_SUCCESS;
}

int git_odb_add_backend(git_odb *odb, git_odb_backend *backend)
{
	assert(odb && backend);

	if (backend->odb != NULL && backend->odb != odb)
		return GIT_EBUSY;

	backend->odb = odb;

	if (git_vector_insert(&odb->backends, backend) < 0)
		return GIT_ENOMEM;

	git_vector_sort(&odb->backends);
	return GIT_SUCCESS;
}


int git_odb_open(git_odb **out, const char *objects_dir)
{
	git_odb *db;
	git_odb_backend *loose, *packed;
	int error;

	if ((error = git_odb_new(&db)) < 0)
		return error;

	/* add the loose object backend */
	if (git_odb_backend_loose(&loose, objects_dir) == 0) {
		error = git_odb_add_backend(db, loose);
		if (error < 0)
			goto cleanup;
	}

	/* add the packed file backend */
	if (git_odb_backend_pack(&packed, objects_dir) == 0) {
		error = git_odb_add_backend(db, packed);
		if (error < 0)
			goto cleanup;
	}

	/* TODO: add altenernates as new backends;
	 * how elevant is that? very elegant. */

	*out = db;
	return GIT_SUCCESS;

cleanup:
	git_odb_close(db);
	return error;
}

void git_odb_close(git_odb *db)
{
	unsigned int i;

	assert(db);

	for (i = 0; i < db->backends.length; ++i) {
		git_odb_backend *b = git_vector_get(&db->backends, i);

		if (b->free) b->free(b);
		else free(b);
	}

	git_vector_free(&db->backends);
	free(db);
}

int git_odb_exists(git_odb *db, const git_oid *id)
{
	unsigned int i;
	int found = 0;

	assert(db && id);

	for (i = 0; i < db->backends.length && !found; ++i) {
		git_odb_backend *b = git_vector_get(&db->backends, i);

		if (b->exists != NULL)
			found = b->exists(b, id);
	}

	return found;
}

int git_odb_read_header(git_rawobj *out, git_odb *db, const git_oid *id)
{
	unsigned int i;
	int error = GIT_ENOTFOUND;

	assert(out && db && id);

	for (i = 0; i < db->backends.length && error < 0; ++i) {
		git_odb_backend *b = git_vector_get(&db->backends, i);

		if (b->read_header != NULL)
			error = b->read_header(out, b, id);
	}

	/* 
	 * no backend could read only the header.
	 * try reading the whole object and freeing the contents
	 */
	if (error < 0) {
		error = git_odb_read(out, db, id);
		git_rawobj_close(out);
	}

	return error;
}

int git_odb_read(git_rawobj *out, git_odb *db, const git_oid *id)
{
	unsigned int i;
	int error = GIT_ENOTFOUND;

	assert(out && db && id);

	for (i = 0; i < db->backends.length && error < 0; ++i) {
		git_odb_backend *b = git_vector_get(&db->backends, i);

		assert(b->read != NULL);
		error = b->read(out, b, id);
	}

	return error;
}

int git_odb_write(git_oid *id, git_odb *db, git_rawobj *obj)
{
	unsigned int i;
	int error = GIT_ERROR;

	assert(obj && db && id);

	for (i = 0; i < db->backends.length && error < 0; ++i) {
		git_odb_backend *b = git_vector_get(&db->backends, i);

		if (b->write != NULL)
			error = b->write(id, b, obj);
	}

	return error;
}

