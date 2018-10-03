/* Pickle: A tiny TCL like interpreter
 *
 *  This is a copy and extension of a tiny TCL interpreter called 'picol', by
 *  Antirez, under the BSD license, available at:
 *
 * <http://oldblog.antirez.com/post/picol.html>
 * <http://antirez.com/picol/picol.c.txt>
 *
 * The BSD license has been moved to the header.
 *
 * Extensions/Changes by Richard James Howe, available at:
 * <https://github.com/howerj/pickle>
 *
 * @todo Allocation changing; custom allocator should be an option, and
 * the allocator running out of memory should be dealt with, as well as
 * proper realloc handling.
 * @todo Add getenv, system, time functions, random function, ...
 * @todo Regexes, Split, Substring, more string operations */
#include "pickle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED(X) ((void)(X))
#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

#define CALLOC(I, SZ)      ((I)->allocator.calloc((I)->allocator.arena,      (SZ)))
#define REALLOC(I, P, SZ) ((I)->allocator.realloc((I)->allocator.arena, (P), (SZ)))
#define FREE(I, P)           ((I)->allocator.free((I)->allocator.arena, (P)))

enum { PICOL_OK, PICOL_ERR, PICOL_RETURN, PICOL_BREAK, PICOL_CONTINUE };
enum { PT_ESC, PT_STR, PT_CMD, PT_VAR, PT_SEP, PT_EOL, PT_EOF };

struct picolParser {
	char *text;
	char *p;         /* current text position */
	int len;         /* remaining length */
	char *start;     /* token start */
	char *end;       /* token end */
	int type;        /* token type, PT_... */
	int insidequote; /* True if inside " " */
};

struct pickle_var {
	char *name, *val;
	struct pickle_var *next;
};

struct pickle_command {
	char *name;
	pickle_command_func_t func;
	void *privdata;
	struct pickle_command *next;
};

struct pickle_call_frame {
	struct pickle_var *vars;
	struct pickle_call_frame *parent; /* parent is NULL at top level */
};

/*static const char *picolIntern(const char *s) {
	static const char *kw[] = {
		"", "if", "proc" // ...
	};
	for(size_t i = 0; i < sizeof(kw)/sizeof(kw[0]); i++)
		if(!strcmp(kw[i], s))
			return kw[i];
	return NULL;
}*/

static char *picolStrdup(allocator_t *a, const char *s) {  /* intern common strings? */
	assert(a);
	assert(s);
	/*char *in = (char*)picolIntern(s); // requires string specific free routine 
	if(in)
		return in;*/
	const size_t l = strlen(s);
	char *r = a->calloc(a->arena, l + 1);
	if(!r) 
		return NULL;
	return memcpy(r, s, l + 1);
}

static void advance(struct picolParser *p) {
	assert(p);
	p->p++; 
	p->len--;
	assert(p->len >= 0);
}

static void picolInitParser(struct picolParser *p, char *text) {
	assert(p);
	assert(text);
	memset(p, 0, sizeof *p);
	p->text = text;
	p->p    = text;
	p->len  = strlen(text);
	p->type = PT_EOL;
}

static inline int picolIsSpaceChar(int ch) {
	return (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
}

static int picolParseSep(struct picolParser *p) {
	assert(p);
	p->start = p->p;
	while(picolIsSpaceChar(*p->p))
		advance(p);
	p->end = p->p - 1;
	p->type = PT_SEP;
	return PICOL_OK;
}

static int picolParseEol(struct picolParser *p) {
	assert(p);
	p->start = p->p;
	while(picolIsSpaceChar(*p->p) || *p->p == ';')
		advance(p);
	p->end = p->p - 1;
	p->type = PT_EOL;
	return PICOL_OK;
}

static int picolParseCommand(struct picolParser *p) {
	assert(p);
	advance(p);
	p->start = p->p; 
	for(int level = 1, blevel = 0; p->len; advance(p)) {
		if (*p->p == '[' && blevel == 0) {
			level++;
		} else if (*p->p == ']' && blevel == 0) {
			if (!--level) break;
		} else if (*p->p == '\\') {
			advance(p);
		} else if (*p->p == '{') {
			blevel++;
		} else if (*p->p == '}') {
			if (blevel != 0) blevel--;
		}
	}
	p->end  = p->p - 1;
	p->type = PT_CMD;
	if (*p->p == ']')
		advance(p);
	return PICOL_OK;
}

static inline int picolIsVarChar(int ch) {
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

static int picolParseVar(struct picolParser *p) {
	assert(p);
	advance(p); /* skip the $ */
	p->start = p->p; 
	for(;picolIsVarChar(*p->p);) 
	       	advance(p);
	if (p->start == p->p) { /* It's just a single char string "$" */
		p->start = p->p - 1;
		p->end   = p->p - 1;
		p->type  = PT_STR;
	} else {
		p->end  = p->p - 1;
		p->type = PT_VAR;
	}
	return PICOL_OK;
}

static int picolParseBrace(struct picolParser *p) {
	assert(p);
	advance(p);
	p->start = p->p; 
	for(int level = 1;;) {
		if (p->len >= 2 && *p->p == '\\') {
			advance(p);
		} else if (p->len == 0 || *p->p == '}') {
			level--;
			if (level == 0 || p->len == 0) {
				p->end = p->p - 1;
				if (p->len)
					advance(p); /* Skip final closed brace */
				p->type = PT_STR;
				return PICOL_OK;
			}
		} else if (*p->p == '{') {
			level++;
		}
		advance(p);
	}
	return PICOL_OK; /* unreached */
}

static int picolParseString(struct picolParser *p) {
	assert(p);
	const int newword = (p->type == PT_SEP || p->type == PT_EOL || p->type == PT_STR);
	if (newword && *p->p == '{') 
		return picolParseBrace(p);
	else if (newword && *p->p == '"') {
		p->insidequote = 1;
		advance(p);
	}
	p->start = p->p;
	for(;p->len;) {
		switch(*p->p) {
		case '\\':
			if (p->len >= 2)
				advance(p);
			break;
		case '$': case '[':
			p->end = p->p - 1;
			p->type = PT_ESC;
			return PICOL_OK;
		case ' ': case '\t': case '\n': case '\r': case ';':
			if (!p->insidequote) {
				p->end = p->p - 1;
				p->type = PT_ESC;
				return PICOL_OK;
			}
			break;
		case '"':
			if (p->insidequote) {
				p->end = p->p - 1;
				p->type = PT_ESC;
				advance(p);
				p->insidequote = 0;
				return PICOL_OK;
			}
			break;
		}
		advance(p);
	}
	p->end = p->p - 1;
	p->type = PT_ESC;
	return PICOL_OK;
}

static int picolParseComment(struct picolParser *p) {
	assert(p);
	while(p->len && *p->p != '\n')
		advance(p);
	return PICOL_OK;
}

static int picolGetToken(struct picolParser *p) {
	assert(p);
	for(;p->len;) {
		switch(*p->p) {
		case ' ': case '\t': case '\r':
			if (p->insidequote) 
				return picolParseString(p);
			return picolParseSep(p);
		case '\n': case ';':
			if (p->insidequote) 
				return picolParseString(p);
			return picolParseEol(p);
		case '[':
			return picolParseCommand(p);
		case '$':
			return picolParseVar(p);
		case '#':
			if (p->type == PT_EOL) {
				picolParseComment(p);
				continue;
			}
			return picolParseString(p);
		default:
			return picolParseString(p);
		}
	}
	if (p->type != PT_EOL && p->type != PT_EOF)
		p->type = PT_EOL;
	else
		p->type = PT_EOF;
	return PICOL_OK;
}

char *pickle_set_result(pickle_t *i, const char *s) {
	assert(i);
	assert(s);
	FREE(i, i->result);
	return i->result = picolStrdup(&i->allocator, s);
}

static struct pickle_var *picolGetVar(pickle_t *i, const char *name) {
	assert(i);
	assert(name);
	struct pickle_var *v = i->callframe->vars;
	while(v) {
		if (!strcmp(v->name, name)) 
			return v;
		v = v->next;
	}
	return NULL;
}

static int picolSetVar(pickle_t *i, char *name, char *val) {
	assert(i);
	assert(name);
	assert(val);
	struct pickle_var *v = picolGetVar(i, name);
	if (v) {
		FREE(i, v->val);
		if(!(v->val = picolStrdup(&i->allocator, val)))
			return PICOL_ERR;
	} else {
		if(!(v = CALLOC(i, sizeof(*v))))
			return PICOL_ERR;
		v->name = picolStrdup(&i->allocator, name);
		v->val  = picolStrdup(&i->allocator, val);
		if(!(v->name) || !(v->val)) {
			FREE(i, v->name);
			FREE(i, v->val);
			FREE(i, v);
			return PICOL_ERR;
		}
		v->next = i->callframe->vars;
		i->callframe->vars = v;
	}
	return PICOL_OK;
}

static struct pickle_command *picolGetCommand(pickle_t *i, const char *name) {
	assert(i);
	assert(name);
	struct pickle_command *c = i->commands;
	while(c) {
		if (!strcmp(c->name, name))
			return c;
		c = c->next;
	}
	return NULL;
}

int pickle_register_command(pickle_t *i, const char *name, pickle_command_func_t f, void *privdata) {
	assert(i);
	assert(name);
	assert(f);
	struct pickle_command *c = picolGetCommand(i, name);
	if (c) {
		char errbuf[1024];
		snprintf(errbuf, sizeof errbuf, "Command '%s' already defined", name);
		pickle_set_result(i, errbuf);
		return PICOL_ERR;
	}
	if(!(c = CALLOC(i, sizeof(*c)))) {
		//pickle_set_result(i, "OOM");
		return PICOL_ERR;
	}
	c->name     = picolStrdup(&i->allocator, name);
	c->func     = f;
	c->privdata = privdata;
	c->next     = i->commands;
	i->commands = c;
	return PICOL_OK;
}

int pickle_eval(pickle_t *i, char *t) {
	assert(i);
	assert(i->initialized);
	assert(t);
	struct picolParser p;
	int retcode = PICOL_OK, argc = 0;
	char **argv = NULL;
	char errbuf[1024];
	pickle_set_result(i, "");
	picolInitParser(&p, t);
	for(;;) {
		int prevtype = p.type;
		picolGetToken(&p);
		if (p.type == PT_EOF) 
			break;
		int tlen = p.end - p.start + 1;
		if (tlen < 0) 
			tlen = 0;
		char *t = CALLOC(i, tlen + 1);
		if(!t) {
			//pickle_set_result(i, "OOM");
			return PICOL_ERR;
		}
		memcpy(t, p.start, tlen);
		t[tlen] = '\0';
		if (p.type == PT_VAR) {
			struct pickle_var *v = picolGetVar(i, t);
			if (!v) {
				snprintf(errbuf, 1024, "No such variable '%s'", t);
				FREE(i, t);
				pickle_set_result(i, errbuf);
				retcode = PICOL_ERR;
				goto err;
			}
			FREE(i, t);
			t = picolStrdup(&i->allocator, v->val);
		} else if (p.type == PT_CMD) {
			retcode = pickle_eval(i, t);
			FREE(i, t);
			if (retcode != PICOL_OK) 
				goto err;
			t = picolStrdup(&i->allocator, i->result);
		} else if (p.type == PT_ESC) {
			/* XXX: escape handling missing! */
		} else if (p.type == PT_SEP) {
			prevtype = p.type;
			FREE(i, t);
			continue;
		}
		/* We have a complete command + args. Call it! */
		if (p.type == PT_EOL) {
			struct pickle_command *c = NULL;
			FREE(i, t);
			prevtype = p.type;
			if (argc) {
				if ((c = picolGetCommand(i, argv[0])) == NULL) {
					snprintf(errbuf, 1024, "No such command '%s'", argv[0]);
					pickle_set_result(i, errbuf);
					retcode = PICOL_ERR;
					goto err;
				}
				retcode = c->func(i, argc, argv, c->privdata);
				if (retcode != PICOL_OK) 
					goto err;
			}
			/* Prepare for the next command */
			for (int j = 0; j < argc; j++) 
				FREE(i, argv[j]);
			FREE(i, argv);
			argv = NULL;
			argc = 0;
			continue;
		}
		/* We have a new token, append to the previous or as new arg? */
		if (prevtype == PT_SEP || prevtype == PT_EOL) {
			char **old = argv;
			argv = REALLOC(i, argv, sizeof(char*)*(argc+1));
			if(!argv) {
				argv = old;
				retcode = PICOL_ERR;
				goto err;
			}
			argv[argc] = t;
			argc++;
		} else { /* Interpolation */
			const int oldlen = strlen(argv[argc - 1]), tlen = strlen(t);
			/**@bug realloc failure memory leak */
			argv[argc - 1] = REALLOC(i, argv[argc - 1], oldlen + tlen + 1);
			memcpy(argv[argc - 1] + oldlen, t, tlen);
			argv[argc - 1][oldlen + tlen]='\0';
			FREE(i, t);
		}
		prevtype = p.type;
	}
err:
	for (int j = 0; j < argc; j++) 
		FREE(i, argv[j]);
	FREE(i, argv);
	return retcode;
}

int pickle_arity_error(pickle_t *i, const char *name) {
	assert(i);
	assert(name);
	char errbuf[1024];
	snprintf(errbuf, sizeof errbuf, "Wrong number of args for %s", name);
	pickle_set_result(i, errbuf);
	return PICOL_ERR;
}

static int picolCommandMath(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	UNUSED(pd);
	if (argc != 3) 
		return pickle_arity_error(i, argv[0]);
	char buf[64]; 
	int a = atoi(argv[1]), b = atoi(argv[2]), c = 0;
	if (argv[0][0] == '+') c = a + b;
	else if (argv[0][0] == '-') c = a - b;
	else if (argv[0][0] == '*') c = a * b;
	else if (argv[0][0] == '/') c = a / b;
	else if (argv[0][0] == '>' && argv[0][1] == '\0') c = a >  b;
	else if (argv[0][0] == '>' && argv[0][1] == '=')  c = a >= b;
	else if (argv[0][0] == '<' && argv[0][1] == '\0') c = a <  b;
	else if (argv[0][0] == '<' && argv[0][1] == '=')  c = a <= b;
	else if (argv[0][0] == '=' && argv[0][1] == '=')  c = a == b;
	else if (argv[0][0] == '!' && argv[0][1] == '=')  c = a != b;
	else exit(-1);
	snprintf(buf, 64, "%d", c);
	pickle_set_result(i, buf);
	return PICOL_OK;
}

static int picolCommandSet(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	UNUSED(pd);
	if (argc != 3) 
		return pickle_arity_error(i, argv[0]);
	picolSetVar(i, argv[1], argv[2]);
	pickle_set_result(i, argv[2]);
	return PICOL_OK;
}

/*From <http://c-faq.com/lib/regex.html>*/
static int match(const char *pat, const char *str) {
	assert(pat);
	assert(str);
	switch(*pat) {
	case '\0':  return !*str;
	case '*':   return match(pat+1, str) || (*str && match(pat, str+1));
	case '.':   return *str && match(pat+1, str+1);
	default:    return *pat == *str && match(pat+1, str+1);
	}
}

static int picolCommandMatch(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	UNUSED(pd);
	if (argc != 3)
		return pickle_arity_error(i, argv[0]);
	pickle_set_result(i, match(argv[1], argv[2]) ? argv[2] : "");
	return PICOL_OK;
}

static int picolCommandIf(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	UNUSED(pd);
	int retcode = 0;
	if (argc != 3 && argc != 5) 
		return pickle_arity_error(i, argv[0]);
	if ((retcode = pickle_eval(i, argv[1])) != PICOL_OK) 
		return retcode;
	if (atoi(i->result)) 
		return pickle_eval(i, argv[2]);
	else if (argc == 5) 
		return pickle_eval(i, argv[4]);
	return PICOL_OK;
}

static int picolCommandWhile(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	UNUSED(pd);
	if (argc != 3) 
		return pickle_arity_error(i, argv[0]);
	for(;;) {
		int retcode = pickle_eval(i, argv[1]);
		if (retcode != PICOL_OK) 
			return retcode;
		if (atoi(i->result)) {
			if ((retcode = pickle_eval(i, argv[2])) == PICOL_CONTINUE) 
				continue;
			else if (retcode == PICOL_OK) 
				continue;
			else if (retcode == PICOL_BREAK) 
				return PICOL_OK;
			else 
				return retcode;
		} else {
			return PICOL_OK;
		}
	}
}

static int picolCommandRetCodes(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	UNUSED(pd);
	if (argc != 1) 
		return pickle_arity_error(i, argv[0]);
	if (!strcmp(argv[0], "break")) 
		return PICOL_BREAK;
	else if (!strcmp(argv[0], "continue"))
		return PICOL_CONTINUE;
	return PICOL_OK;
}

static void picolDropCallFrame(pickle_t *i) {
	assert(i);
	struct pickle_call_frame *cf = i->callframe;
	struct pickle_var *v = cf->vars, *t = NULL;
	while(v) {
		t = v->next;
		FREE(i, v->name);
		FREE(i, v->val);
		FREE(i, v);
		v = t;
	}
	i->callframe = cf->parent;
	FREE(i, cf);
}

static int picolCommandCallProc(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	char **x = pd, *alist = x[0], *body = x[1], *p = picolStrdup(&i->allocator, alist), *tofree = NULL;
	int arity = 0, done = 0, errcode = PICOL_OK;
	char errbuf[1024];
	if(!p) {
		//pickle_set_result(i, "OOM");
		return PICOL_ERR;
	}
	struct pickle_call_frame *cf = CALLOC(i, sizeof(*cf));
	if(!cf) {
		//pickle_set_result(i, "OOM");
		FREE(i, p);
		return PICOL_ERR;
	}
	cf->vars = NULL;
	cf->parent = i->callframe;
	i->callframe = cf;
	tofree = p;
	for(;;) {
		char *start = p;
		while(*p != ' ' && *p != '\0') 
			p++;
		if (*p != '\0' && p == start) {
			p++; 
			continue;
		}
		if (p == start) 
			break;
		if (*p == '\0') 
			done = 1; 
		else 
			*p = '\0';
		if (++arity > (argc - 1)) 
			goto arityerr;
		picolSetVar(i, start, argv[arity]);
		p++;
		if (done) 
			break;
	}
	FREE(i, tofree);
	if (arity != argc - 1) 
		goto arityerr;
	errcode = pickle_eval(i, body);
	if (errcode == PICOL_RETURN) 
		errcode = PICOL_OK;
	picolDropCallFrame(i); /* remove the called proc callframe */
	return errcode;
arityerr:
	snprintf(errbuf, 1024, "Proc '%s' called with wrong arg num", argv[0]);
	pickle_set_result(i, errbuf);
	picolDropCallFrame(i); /* remove the called proc callframe */
	return PICOL_ERR;
}

static int picolCommandProc(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	UNUSED(pd);
	if (argc != 4) 
		return pickle_arity_error(i, argv[0]);
	char **procdata = CALLOC(i, sizeof(char*)*2);
	if(!procdata) {
		return PICOL_ERR;
	}
	procdata[0] = picolStrdup(&i->allocator, argv[2]); /* arguments list */
	procdata[1] = picolStrdup(&i->allocator, argv[3]); /* procedure body */
	if(!(procdata[0]) || !(procdata[1])) {
		FREE(i, procdata[0]);
		FREE(i, procdata[1]);
		FREE(i, procdata);
		return PICOL_ERR;
	}
	return pickle_register_command(i, argv[1], picolCommandCallProc, procdata);
}

static int picolCommandReturn(pickle_t *i, int argc, char **argv, void *pd) {
	UNUSED(pd);
	if (argc != 1 && argc != 2) 
		return pickle_arity_error(i, argv[0]);
	pickle_set_result(i, (argc == 2) ? argv[1] : "");
	return PICOL_RETURN;
}

int picolRegisterCoreCommands(pickle_t *i) {
	assert(i);
	typedef struct {
		char *name;
		pickle_command_func_t func;
		void *data;
	} pickle_command_t;
	const pickle_command_t commands[] = {
		{ "set",       picolCommandSet,       NULL}, 
		{ "if",        picolCommandIf,        NULL}, 
		{ "while",     picolCommandWhile,     NULL}, 
		{ "break",     picolCommandRetCodes,  NULL}, 
		{ "continue",  picolCommandRetCodes,  NULL}, 
		{ "proc",      picolCommandProc,      NULL}, 
		{ "return",    picolCommandReturn,    NULL}, 
		{ "match",     picolCommandMatch,     NULL}, 
	};
	static const char *name[] = { "+", "-", "*", "/", ">", ">=", "<", "<=", "==", "!=" };
	for (size_t j = 0; j < sizeof(name)/sizeof(char*); j++)
		if (pickle_register_command(i, name[j], picolCommandMath, NULL) != PICOL_OK)
			return -1;
	for (size_t j = 0; j < sizeof(commands)/sizeof(commands[0]); j++)
		if (pickle_register_command(i, commands[j].name, commands[j].func, commands[j].data) != PICOL_OK)
			return -1;
	return 0;
}

static void pickleFreeCmd(allocator_t *a, struct pickle_command *p) {
	assert(a);
	if(!p)
		return;
	if(p->func == picolCommandCallProc) {
		char **procdata = (char**) p->privdata;
		if(procdata) {
			a->free(a->arena, procdata[0]);
			a->free(a->arena, procdata[1]);
		}
		a->free(a->arena, procdata);
	}
	a->free(a->arena, p->name);
	a->free(a->arena, p);
}

int pickle_deinitialize(pickle_t *i) {
	assert(i);
	allocator_t *a = &i->allocator;
	i->initialized = 0;
	picolDropCallFrame(i);
	a->free(a->arena, i->result);
	struct pickle_command *c = i->commands, *p = NULL;
	for(; c; p = c, c = c->next)
		pickleFreeCmd(a, p);
	pickleFreeCmd(a, p);
	return 0;
}

static void *pcalloc(void *arena, size_t size) {
	UNUSED(arena);
	return calloc(size, 1);
}

static void *prealloc(void *arena, void *ptr, size_t size) {
	UNUSED(arena);
	return realloc(ptr, size);
}

static void pfree(void *arena, void *ptr) {
	UNUSED(arena);
	free(ptr);
}

static allocator_t allocator = {
	.calloc  = pcalloc,
	.realloc = prealloc,
	.free    = pfree,
	.arena   = NULL,
};

int pickle_initialize(pickle_t *i, allocator_t *a) {
	assert(i);
	assert(!(i->initialized));
	memset(i, 0, sizeof *i);
	i->initialized = 1;
	i->allocator   = a ? *a : allocator;
	i->callframe   = i->allocator.calloc(i->allocator.arena, sizeof(struct pickle_call_frame));
	i->result      = picolStrdup(&i->allocator, "");
	if(!(i->callframe) || !(i->result))
		goto fail;
	if(picolRegisterCoreCommands(i) < 0)
		goto fail;
	return 0;
fail:
	pickle_deinitialize(i);
	return -1;
}

