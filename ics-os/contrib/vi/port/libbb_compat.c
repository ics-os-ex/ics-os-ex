#include "include/libbb.h"

int optind = 1;

struct globals *ptr_to_globals = NULL;

extern int vi_main(int argc, char **argv);

static int is_option_char(char c)
{
	return (c != '\0' && c != ':' && c != '*');
}

static int count_args(char **argv)
{
	int i = 0;
	while (argv[i]) {
		i++;
	}
	return i;
}

int getopt32(char **argv, const char *optstring, ...)
{
	int argc = count_args(argv);
	int opts = 0;
	int i;
	char opt_map[64];
	unsigned char opt_has_arg[64];
	unsigned char opt_is_list[64];
	int opt_count = 0;
	int idx;
	va_list ap;
	void *arg_targets[64];
	unsigned char arg_is_list[64];
	int arg_target_count = 0;

	memset(opt_map, 0, sizeof(opt_map));
	memset(opt_has_arg, 0, sizeof(opt_has_arg));
	memset(opt_is_list, 0, sizeof(opt_is_list));

	for (i = 0; optstring[i]; i++) {
		char c = optstring[i];
		if (!is_option_char(c))
			continue;
		if (opt_count < (int)sizeof(opt_map)) {
			opt_map[opt_count] = c;
			opt_has_arg[opt_count] = (optstring[i + 1] == ':');
			opt_is_list[opt_count] = (opt_has_arg[opt_count] && optstring[i + 2] == '*');
			opt_count++;
		}
	}

	va_start(ap, optstring);
	for (i = 0; i < opt_count; i++) {
		if (opt_has_arg[i]) {
			arg_targets[arg_target_count] = va_arg(ap, void *);
			arg_is_list[arg_target_count] = opt_is_list[i];
			arg_target_count++;
		}
	}
	va_end(ap);

	optind = 1;
	while (optind < argc) {
		char *arg = argv[optind];
		if (!arg || arg[0] != '-' || arg[1] == '\0')
			break;
		if (arg[0] == '-' && arg[1] == '-' && arg[2] == '\0') {
			optind++;
			break;
		}

		for (i = 1; arg[i]; i++) {
			char c = arg[i];
			for (idx = 0; idx < opt_count; idx++) {
				if (opt_map[idx] == c) {
					opts |= (1 << idx);
					if (opt_has_arg[idx]) {
						char *val = NULL;
						if (arg[i + 1]) {
							val = &arg[i + 1];
							i = (int)strlen(arg) - 1;
						} else if (optind + 1 < argc) {
							val = argv[++optind];
						}
						if (arg_target_count > 0) {
							if (arg_is_list[0]) {
								llist_t **head = (llist_t **)arg_targets[0];
								llist_t *node = xzalloc(sizeof(llist_t));
								node->data = xstrdup(val ? val : "");
								node->next = *head;
								*head = node;
							} else {
								char **out = (char **)arg_targets[0];
								*out = val;
							}
						}
					}
					break;
				}
			}
		}
		optind++;
	}

	return opts;
}

char *llist_pop(llist_t **head)
{
	llist_t *node;
	char *data;

	if (!head || !*head)
		return NULL;
	node = *head;
	*head = node->next;
	data = node->data;
	free(node);
	return data;
}

void bb_simple_error_msg_and_die(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

void bb_show_usage(void)
{
	fprintf(stderr, "usage: vi [file]\n");
}

void bb_putchar(int c)
{
	putchar(c);
}

unsigned long bb_strtou(const char *arg, char **endp, int base)
{
	unsigned long val = 0;
	const char *p = arg;

	if (!p)
		return 0;

	if (base != 10 && base != 0)
		base = 10;

	while (*p == ' ' || *p == '\t')
		p++;

	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			break;
		val = val * 10 + (unsigned long)(*p - '0');
	}

	if (endp)
		*endp = (char *)p;
	return val;
}

void *xmalloc(size_t size)
{
	void *p = malloc(size);
	if (!p)
		bb_simple_error_msg_and_die("out of memory");
	return p;
}

void *xzalloc(size_t size)
{
	void *p = xmalloc(size);
	memset(p, 0, size);
	return p;
}

void *xrealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);
	if (!p)
		bb_simple_error_msg_and_die("out of memory");
	return p;
}

char *xstrdup(const char *s)
{
	char *d;
	if (!s)
		return NULL;
	d = xmalloc(strlen(s) + 1);
	strcpy(d, s);
	return d;
}

char *xstrndup(const char *s, int n)
{
	char *d;
	int len = 0;
	if (!s)
		return NULL;
	while (len < n && s[len])
		len++;
	d = xmalloc(len + 1);
	memcpy(d, s, len);
	d[len] = '\0';
	return d;
}

char *xasprintf(const char *fmt, ...)
{
	char tmp[2048];
	va_list ap;
	if (!fmt)
		return xstrdup("");

	va_start(ap, fmt);
	vsprintf(tmp, fmt, ap);
	va_end(ap);

	return xstrdup(tmp);
}

char *xmalloc_open_read_close(const char *filename, size_t *sizep)
{
	FILE *f;
	vfs_stat st;
	char *buf;
	int readlen;

	f = fopen(filename, "r");
	if (!f)
		return NULL;

	if (fstat(f, &st) != 0) {
		fclose(f);
		return NULL;
	}

	buf = xmalloc(st.st_size + 1);
	readlen = fread(buf, 1, st.st_size, f);
	buf[readlen] = '\0';
	if (sizep)
		*sizep = readlen;
	fclose(f);
	return buf;
}

char *concat_path_file(const char *path, const char *filename)
{
	int need_sep;
	char *out;
	int len_path;
	int len_file;

	if (!path)
		path = "";
	if (!filename)
		filename = "";

	len_path = strlen(path);
	len_file = strlen(filename);
	need_sep = (len_path > 0 && path[len_path - 1] != '/');

	out = xmalloc(len_path + len_file + (need_sep ? 2 : 1));
	strcpy(out, path);
	if (need_sep)
		strcat(out, "/");
	strcat(out, filename);
	return out;
}

void fputs_stdout(const char *s)
{
	if (s)
		fputs(s, stdout);
}

void fflush_all(void)
{
	fflush(stdout);
	fflush(stderr);
}

int safe_read_key(int fd, char *buf, int timeout)
{
	(void)timeout;
	if (safe_read(fd, buf, 1) != 1)
		return -1;
	return (unsigned char)buf[0];
}

int safe_poll(struct pollfd *pfd, int nfds, int timeout)
{
	(void)pfd;
	(void)nfds;
	if (timeout > 0)
		sleep((unsigned int)timeout);
	return 0;
}

ssize_t safe_read(int fd, void *buf, size_t count)
{
	return read(fd, buf, count);
}

ssize_t full_read(int fd, void *buf, size_t count)
{
	size_t done = 0;
	char *p = (char *)buf;

	while (done < count) {
		ssize_t rc = read(fd, p + done, count - done);
		if (rc <= 0)
			break;
		done += rc;
	}
	return (ssize_t)done;
}

ssize_t full_write(int fd, const void *buf, size_t count)
{
	size_t done = 0;
	const char *p = (const char *)buf;

	while (done < count) {
		ssize_t rc = write(fd, p + done, count - done);
		if (rc <= 0)
			break;
		done += rc;
	}
	return (ssize_t)done;
}

int set_termios_to_raw(int fd, struct termios *old, int flags)
{
	(void)fd;
	(void)old;
	(void)flags;
	return 0;
}

int tcsetattr_stdin_TCSANOW(const struct termios *tp)
{
	(void)tp;
	return 0;
}

int vsnprintf(char *buffer, int size, const char *fmt, va_list args)
{
	char tmp[2048];
	int len;

	if (!buffer || size <= 0)
		return 0;
	if (!fmt) {
		buffer[0] = '\0';
		return 0;
	}

	len = vsprintf(tmp, fmt, args);
	if (len < 0)
		return len;
	if (len >= size)
		len = size - 1;
	memcpy(buffer, tmp, len);
	buffer[len] = '\0';
	return len;
}

int snprintf(char *buffer, int size, const char *fmt, ...)
{
	va_list ap;
	int len;
	if (!fmt) {
		if (buffer && size > 0)
			buffer[0] = '\0';
		return 0;
	}

	va_start(ap, fmt);
	len = vsnprintf(buffer, size, fmt, ap);
	va_end(ap);
	return len;
}

int isblank(int c)
{
	return (c == ' ' || c == '\t');
}

int ispunct(int c)
{
	return ((c >= 33 && c <= 47) || (c >= 58 && c <= 64)
		|| (c >= 91 && c <= 96) || (c >= 123 && c <= 126));
}

int open(const char *path, int flags, ...)
{
	int mode = FILE_READ;

	if (flags & O_RDWR)
		mode = FILE_READWRITE;
	else if (flags & O_WRONLY)
		mode = FILE_WRITE;
	else if (flags & O_APPEND)
		mode = FILE_APPEND;

	return (int)openfile(path, mode);
}

int close(int fd)
{
	return fclose((FILE *)fd);
}

ssize_t read(int fd, void *buf, size_t count)
{
	unsigned char *p = (unsigned char *)buf;
	size_t i;

	if (fd == STDIN_FILENO) {
		for (i = 0; i < count; i++) {
			p[i] = (unsigned char)getch();
		}
		return (ssize_t)count;
	}

	return (ssize_t)fread((char *)buf, 1, (int)count, (FILE *)fd);
}

ssize_t write(int fd, const void *buf, size_t count)
{
	FILE *out = NULL;

	if (fd == STDOUT_FILENO)
		out = stdout;
	else if (fd == STDERR_FILENO)
		out = stderr;
	else
		out = (FILE *)fd;

	return (ssize_t)fwrite((char *)buf, 1, (int)count, out);
}

int ftruncate(int fd, unsigned int length)
{
	(void)fd;
	(void)length;
	return 0;
}

int stat(const char *path, struct stat *st)
{
	vfs_stat vs;
	int rc;

	rc = dexsdk_systemcall(36, (int)path, (int)&vs, 0, 0, 0);
	if (rc != 0)
		return -1;
	memcpy(st, &vs, sizeof(vs));
	return 0;
}

int getuid(void)
{
	return 0;
}

int system(const char *cmd)
{
	(void)cmd;
	return -1;
}

int main(int argc, char **argv)
{
	return vi_main(argc, argv);
}

void *memrchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i;

	for (i = n; i > 0; i--) {
		if (p[i - 1] == (unsigned char)c)
			return (void *)(p + i - 1);
	}
	return NULL;
}

char *strchrnul(const char *s, int c)
{
	const char *p = s;
	if (!p)
		return NULL;
	while (*p && *p != c)
		p++;
	return (char *)p;
}
