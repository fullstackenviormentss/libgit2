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
#include "test_helpers.h"
#include "fileops.h"
#include "git/oid.h"
#include "git/repository.h"

int write_object_data(char *file, void *data, size_t len)
{
	git_file fd;
	int ret;

	if ((fd = gitfo_creat(file, S_IREAD | S_IWRITE)) < 0)
		return -1;
	ret = gitfo_write(fd, data, len);
	gitfo_close(fd);

	return ret;
}

int write_object_files(const char *odb_dir, object_data *d)
{
	if (gitfo_mkdir(odb_dir, 0755) < 0) {
		int err = errno;
		fprintf(stderr, "can't make directory \"%s\"", odb_dir);
		if (err == EEXIST)
			fprintf(stderr, " (already exists)");
		fprintf(stderr, "\n");
		return -1;
	}

	if ((gitfo_mkdir(d->dir, 0755) < 0) && (errno != EEXIST)) {
		fprintf(stderr, "can't make object directory \"%s\"\n", d->dir);
		return -1;
	}
	if (write_object_data(d->file, d->bytes, d->blen) < 0) {
		fprintf(stderr, "can't write object file \"%s\"\n", d->file);
		return -1;
	}

	return 0;
}

int remove_object_files(const char *odb_dir, object_data *d)
{
	if (gitfo_unlink(d->file) < 0) {
		fprintf(stderr, "can't delete object file \"%s\"\n", d->file);
		return -1;
	}
	if ((gitfo_rmdir(d->dir) < 0) && (errno != ENOTEMPTY)) {
		fprintf(stderr, "can't remove object directory \"%s\"\n", d->dir);
		return -1;
	}

	if (gitfo_rmdir(odb_dir) < 0) {
		fprintf(stderr, "can't remove directory \"%s\"\n", odb_dir);
		return -1;
	}

	return 0;
}

int remove_loose_object(const char *repository_folder, git_object *object)
{
	static const char *objects_folder = "objects/";

	char *ptr, *full_path, *top_folder;
	int path_length, objects_length;

	assert(repository_folder && object);

	objects_length = strlen(objects_folder);
	path_length = strlen(repository_folder);
	ptr = full_path = git__malloc(path_length + objects_length + GIT_OID_HEXSZ + 3);

	strcpy(ptr, repository_folder);
	strcpy(ptr + path_length, objects_folder);

	ptr = top_folder = ptr + path_length + objects_length;
	*ptr++ = '/';
	git_oid_pathfmt(ptr, git_object_id(object));
	ptr += GIT_OID_HEXSZ + 1;
	*ptr = 0;

	if (gitfo_unlink(full_path) < 0) {
		fprintf(stderr, "can't delete object file \"%s\"\n", full_path);
		return -1;
	}

	*top_folder = 0;

	if ((gitfo_rmdir(full_path) < 0) && (errno != ENOTEMPTY)) {
		fprintf(stderr, "can't remove object directory \"%s\"\n", full_path);
		return -1;
	}

	free(full_path);

	return GIT_SUCCESS;
}

int cmp_objects(git_rawobj *o, object_data *d)
{
	if (o->type != git_otype_fromstring(d->type))
		return -1;
	if (o->len != d->dlen)
		return -1;
	if ((o->len > 0) && (memcmp(o->data, d->data, o->len) != 0))
		return -1;
	return 0;
}
