/*
 * Copyright (c) 2008 Vincent Bernat <bernat@luffy.cx>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "lldpd.h"

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

int
ctl_create(struct lldpd *cfg, char *name)
{
	int s;
	struct sockaddr_un su;
	int rc;

	if ((s = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;
	su.sun_family = AF_UNIX;
	strlcpy(su.sun_path, name, UNIX_PATH_MAX);
	if (bind(s, (struct sockaddr *)&su, sizeof(struct sockaddr_un)) == -1) {
		rc = errno; close(s); errno = rc;
		return -1;
	}
	if (listen(s, 5) == -1) {
		rc = errno; close(s); errno = rc;
		return -1;
	}
	TAILQ_INIT(&cfg->g_clients);
	return s;
}

int
ctl_connect(char *name)
{
	int s;
	struct sockaddr_un su;
	int rc;

	if ((s = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;
	su.sun_family = AF_UNIX;
	strlcpy(su.sun_path, name, UNIX_PATH_MAX);
	if (connect(s, (struct sockaddr *)&su, sizeof(struct sockaddr_un)) == -1) {
		rc = errno;
		LLOG_WARN("unable to connect to socket " LLDPD_CTL_SOCKET);
		errno = rc; return -1;
	}
	return s;
}

int
ctl_accept(struct lldpd *cfg, int c)
{
	int s;
	struct lldpd_client *lc;
	if ((s = accept(c, NULL, NULL)) == -1) {
		LLOG_WARN("unable to accept connection from socket");
		return -1;
	}
	if ((lc = (struct lldpd_client *)malloc(sizeof(
			    struct lldpd_client))) == NULL) {
		LLOG_WARN("failed to allocate memory for new client");
		close(s);
		return -1;
	}
	lc->fd = s;
	TAILQ_INSERT_TAIL(&cfg->g_clients, lc, next);
	return 1;
}

void
ctl_msg_init(struct hmsg *t, enum hmsg_type type)
{
	t->hdr.type = type;
	t->hdr.len = 0;
	t->hdr.pid = getpid();
}

int
ctl_msg_send(int fd, struct hmsg *t)
{
	return write(fd, t, t->hdr.len + sizeof(struct hmsg_hdr));
}

int
ctl_msg_recv(int fd, struct hmsg *t)
{
	int n;
	if ((n = read(fd, t, MAX_HMSGSIZE)) == -1) {
		return -1;
	}
	if (n < sizeof(struct hmsg_hdr)) {
		LLOG_WARNX("message received too short");
		errno = 0;
		return -1;
	}
	if (n != sizeof(struct hmsg_hdr) + t->hdr.len) {
		LLOG_WARNX("message from %d seems to be truncated (or too large)",
			t->hdr.pid);
		errno = 0;
		return -1;
	}
	return 1;
}

int
ctl_close(struct lldpd *cfg, int c)
{
	struct lldpd_client *client, *client_next;
	for (client = TAILQ_FIRST(&cfg->g_clients);
	    client != NULL;
	    client = client_next) {
		client_next = TAILQ_NEXT(client, next);
		if (client->fd == c) {
			close(client->fd);
			TAILQ_REMOVE(&cfg->g_clients, client, next);
			free(client);
			return 1;
		}
	}
	/* Not found */
	return -1;
}

void
ctl_cleanup(int c, char *name)
{
	close(c);
	if (unlink(name) == -1)
		LLOG_WARN("unable to unlink %s", name);
}

/* Packing/unpacking */

/* This structure is used to track memory allocation when unpacking */
struct gc {
	TAILQ_ENTRY(gc) next;
	void *pointer;
};
TAILQ_HEAD(gc_l, gc);

typedef struct { char c; int16_t x; } st_int16;
typedef struct { char c; int32_t x; } st_int32;
typedef struct { char c; void *x; } st_void_p;

#define INT16_ALIGN (sizeof(st_int16) - sizeof(int16_t))
#define INT32_ALIGN (sizeof(st_int32) - sizeof(int32_t))
#define VOID_P_ALIGN (sizeof(st_void_p) - sizeof(void *))

struct formatdef {
	char format;
	int  size;
	int  alignment;
	int (*pack)(struct hmsg*, void **, void *,
	    const struct formatdef *);
	int (*unpack)(struct hmsg*, void **, void *,
	    const struct formatdef *, struct gc_l *);
};

/* void** is a pointer to a pointer to the end of struct hmsg*. It should be
 * updated. void* is a pointer to the entity to pack */

int
_ctl_alloc_pointer(struct gc_l *pointers, void *pointer)
{
	struct gc *gpointer;
	if (pointers != NULL) {
		if ((gpointer = (struct gc *)calloc(1,
			    sizeof(struct gc))) == NULL) {
			LLOG_WARN("unable to allocate memory for garbage collector");
			return -1;
		}
		gpointer->pointer = pointer;
		TAILQ_INSERT_TAIL(pointers, gpointer, next);
	}
	return 0;
}

void
_ctl_free_pointers(struct gc_l *pointers, int listonly)
{
	struct gc *pointer, *pointer_next;
	for (pointer = TAILQ_FIRST(pointers);
	     pointer != NULL;
	     pointer = pointer_next) {
		pointer_next = TAILQ_NEXT(pointer, next);
		TAILQ_REMOVE(pointers, pointer, next);
		if (!listonly)
			free(pointer->pointer);
		free(pointer);
	}
}

int
pack_copy(struct hmsg *h, void **p, void *s,
    const struct formatdef *ct)
{
	if (h->hdr.len + ct->size > MAX_HMSGSIZE - sizeof(struct hmsg_hdr)) {
		LLOG_WARNX("message became too large");
		return -1;
	}
	memcpy(*p, s, ct->size);
	*p += ct->size;
	h->hdr.len += ct->size;
	return ct->size;
}

int
unpack_copy(struct hmsg *h, void **p, void *s,
    const struct formatdef *ct, struct gc_l *pointers)
{
	memcpy(s, *p, ct->size);
	*p += ct->size;
	return ct->size;
}

int
pack_string(struct hmsg *h, void **p, void *s,
    const struct formatdef *ct)
{
	int len = strlen(*(char**)s);
	if (h->hdr.len + len + sizeof(int) > MAX_HMSGSIZE -
	    sizeof(struct hmsg_hdr)) {
		LLOG_WARNX("message became too large");
		return -1;
	}
	memcpy(*p, &len, sizeof(int));
	*p += sizeof(int);
	memcpy(*p, *(char **)s, len);
	*p += len;
	h->hdr.len += sizeof(int) + len;
	return sizeof(int) + len;
}

int
unpack_string(struct hmsg *h, void **p, void *s,
    const struct formatdef *ct, struct gc_l *pointers)
{
	char *string;
	int len = *(int*)*p;
	*p += sizeof(int);
	if ((string = (char *)calloc(1, len + 1)) == NULL) {
		LLOG_WARNX("unable to allocate new string");
		return -1;
	}
	if (_ctl_alloc_pointer(pointers, string) == -1) {
		free(string);
		return -1;
	}
	memcpy(string, *p, len);
	*p += len;
	memcpy(s, &string, sizeof(char *));
	return sizeof(char*);
}

int
pack_chars(struct hmsg *h, void **p, void *s,
    const struct formatdef *ct)
{
	char *string;
	int string_len;
	string = *(char **)s;
	s += sizeof(char *);
	string_len = *(int *)s;

	if (h->hdr.len + string_len + sizeof(int) > MAX_HMSGSIZE -
	    sizeof(struct hmsg_hdr)) {
		LLOG_WARNX("message became too large");
		return -1;
	}
	memcpy(*p, &string_len, sizeof(int));
	*p += sizeof(int);
	memcpy(*p, string, string_len);
	*p += string_len;
	h->hdr.len += sizeof(int) + string_len;
	return sizeof(int) + string_len;
}

int
unpack_chars(struct hmsg *h, void **p, void *s,
    const struct formatdef *ct, struct gc_l *pointers)
{
	char *string;
	struct {
		char *string;
		int len;
	} reals __attribute__ ((__packed__));
	int len = *(int*)*p;
	*p += sizeof(int);
	if ((string = (char *)malloc(len)) == NULL) {
		LLOG_WARN("unable to allocate new string");
		return -1;
	}
	if (_ctl_alloc_pointer(pointers, string) == -1) {
		free(string);
		return -1;
	}
	memcpy(string, *p, len);
	*p += len;
	reals.string = string;
	reals.len = len;
	memcpy(s, &reals, sizeof(reals));
	return sizeof(char*);
}

int
pack_zero(struct hmsg *h, void **p, void *s,
    const struct formatdef *ct)
{
	if (h->hdr.len + ct->size > MAX_HMSGSIZE - sizeof(struct hmsg_hdr)) {
		LLOG_WARNX("message became too large");
		return -1;
	}
	memset(*p, 0, ct->size);
	*p += ct->size;
	h->hdr.len += ct->size;
	return ct->size;
}

static struct formatdef conv_table[] = {
	{'b',	1,				0,
	 pack_copy,	unpack_copy},
	{'w',	2,				INT16_ALIGN,
	 pack_copy,	unpack_copy},
	{'l',	4,				INT32_ALIGN,
	 pack_copy,	unpack_copy},
	/* Null terminated string */
	{'s',	sizeof(void*),			VOID_P_ALIGN,
	 pack_string,	unpack_string},
	/* Pointer (is packed with 0) */
	{'P',	sizeof(void*),			VOID_P_ALIGN,
	 pack_zero,	unpack_copy},
	/* A list (same as pointer), should be at the beginning */
	{'L',	sizeof(void*)*2,		VOID_P_ALIGN,
	 pack_zero,	unpack_copy},
	/* Non null terminated string, followed by an int for the size */
	{'C',	sizeof(void*) + sizeof(int),	VOID_P_ALIGN,
	 pack_chars,	unpack_chars},
	{0}
};

/* Lists can be packed only if the "next" member is the first one of the
 * structure! No check is done for this. */
struct fakelist_m {
	TAILQ_ENTRY(fakelist_m)	 next;
	void *data;
};
TAILQ_HEAD(fakelist_l, fakelist_m);

int
ctl_msg_pack_structure(char *format, void *structure, unsigned int size,
    struct hmsg *h, void **p)
{
	char *f;
	struct formatdef *ce;
	unsigned int csize = 0;
	uintptr_t offset;
	int maxalign = 0;
	
	for (f = format; *f != 0; f++) {
		for (ce = conv_table;
		     ce->format != 0;
		     ce++) {
			if (ce->format == *f)
				break;
		}
		if (ce->format != *f) {
			LLOG_WARNX("unknown format char %c in %s", *f,
			    format);
			return -1;
		}
		if (ce->alignment != 0) {
			maxalign = (maxalign>ce->alignment)?maxalign:ce->alignment;
			offset = (uintptr_t)structure % ce->alignment;
			if (offset != 0) {
				structure = structure +
				    (ce->alignment - offset);
				csize += ce->alignment - offset;
			}
		}
		if (ce->pack(h, p, structure, ce) == -1) {
			LLOG_WARNX("error while packing %c in %s", *f,
			    format);
			return -1;
		}
		structure += ce->size;
		csize += ce->size;
	}

	/* End padding */
	if ((maxalign > 0) && (csize % maxalign != 0))
		csize = csize + maxalign - (csize % maxalign);
	if (size != csize) {
		LLOG_WARNX("size of structure does not match its "
		    "declaration (%d vs %d)", size, csize);
		return -1;
	}
	return 0;
}

int
_ctl_msg_unpack_structure(char *format, void *structure, unsigned int size,
    struct hmsg *h, void **p, struct gc_l *pointers)
{
	char *f;
	struct formatdef *ce;
	unsigned int csize = 0;
	uintptr_t offset;
	int maxalign = 0;

	for (f = format; *f != 0; f++) {
		for (ce = conv_table;
		     ce->format != 0;
		     ce++) {
			if (ce->format == *f)
				break;
		}
		if (ce->format != *f) {
			LLOG_WARNX("unknown format char %c", *f);
			return -1;
		}
		if (ce->alignment != 0) {
			maxalign = (maxalign>ce->alignment)?maxalign:ce->alignment;
			offset = (uintptr_t)structure % ce->alignment;
			if (offset != 0) {
				structure = structure +
				    (ce->alignment - offset);
				csize += ce->alignment - offset;
			}
		}
		csize += ce->size;
		if (csize > size) {
			LLOG_WARNX("size of structure is too small for given "
			    "format (%d vs %d)", size, csize);
			return -1;
		}
			
		if (ce->unpack(h, p, structure, ce, pointers) == -1) {
			LLOG_WARNX("error while unpacking %c", *f);
			return -1;
		}
		structure += ce->size;
	}

	/* End padding */
	if ((maxalign > 0) && (csize % maxalign != 0))
		csize = csize + maxalign - (csize % maxalign);
	if (size < csize) {
		LLOG_WARNX("size of structure does not match its "
		    "declaration (%d vs %d)", size, csize);
		return -1;
	}
	return 0;
}

int
ctl_msg_unpack_structure(char *format, void *structure, unsigned int size,
    struct hmsg *h, void **p)
{
	struct gc_l pointers;
	int rc;
	TAILQ_INIT(&pointers);
	if ((rc = _ctl_msg_unpack_structure(format, structure, size,
		    h, p, &pointers)) == -1) {
		LLOG_WARNX("unable to unpack structure, freeing");
		_ctl_free_pointers(&pointers, 0);
		return -1;
	}
	_ctl_free_pointers(&pointers, 1);
	return rc;
}

int
ctl_msg_pack_list(char *format, void *list, unsigned int size, struct hmsg *h, void **p)
{
	struct fakelist_m *member;
	struct fakelist_l *flist = (struct fakelist_l *)list;
	TAILQ_FOREACH(member, flist, next) {
		if (ctl_msg_pack_structure(format, member, size, h, p) == -1) {
			LLOG_WARNX("error while packing list, aborting");
			return -1;
		}
	}
	return 0;
}

int
ctl_msg_unpack_list(char *format, void *list, unsigned int size, struct hmsg *h, void **p)
{
	struct fakelist_m *member, *member_next;
	struct gc_l pointers;
	struct fakelist_l *flist = (struct fakelist_l *)list;
	if (format[0] != 'L') {
		LLOG_WARNX("to unpack a list, format should start with 'L'");
		return -1;
	}
	TAILQ_INIT(flist);
	TAILQ_INIT(&pointers);
	while (*p - (void *)h - sizeof(struct hmsg_hdr) < h->hdr.len) {
		if ((member = calloc(1, size)) == NULL) {
			LLOG_WARN("unable to allocate memory for structure");
			return -1;
		}
		if (_ctl_msg_unpack_structure(format, member, size,
			h, p, &pointers) == -1) {
			LLOG_WARNX("unable to unpack list, aborting");
			free(member);
			/* Free each list member */
			for (member = TAILQ_FIRST(flist);
			     member != NULL;
			     member = member_next) {
				member_next = TAILQ_NEXT(member, next);
				TAILQ_REMOVE(flist, member, next);
				free(member);
			}
			_ctl_free_pointers(&pointers, 0);
			return -1;
		}
		TAILQ_INSERT_TAIL(flist, member, next);
	}
	_ctl_free_pointers(&pointers, 1);
	return 0;
}