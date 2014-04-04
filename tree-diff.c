/*
 * Helper functions for tree diff generation
 */
#include "cache.h"
#include "diff.h"
#include "diffcore.h"
#include "tree.h"


static int ll_diff_tree_sha1(const unsigned char *old, const unsigned char *new,
			     struct strbuf *base, struct diff_options *opt);

/*
 * Compare two tree entries, taking into account only path/S_ISDIR(mode),
 * but not their sha1's.
 *
 * NOTE files and directories *always* compare differently, even when having
 *      the same name - thanks to base_name_compare().
 *
 * NOTE empty (=invalid) descriptor(s) take part in comparison as +infty,
 *      so that they sort *after* valid tree entries.
 *
 *      Due to this convention, if trees are scanned in sorted order, all
 *      non-empty descriptors will be processed first.
 */
static int tree_entry_pathcmp(struct tree_desc *t1, struct tree_desc *t2)
{
	struct name_entry *e1, *e2;
	int cmp;

	/* empty descriptors sort after valid tree entries */
	if (!t1->size)
		return t2->size ? 1 : 0;
	else if (!t2->size)
		return -1;

	e1 = &t1->entry;
	e2 = &t2->entry;
	cmp = base_name_compare(e1->path, tree_entry_len(e1), e1->mode,
				e2->path, tree_entry_len(e2), e2->mode);
	return cmp;
}


/* convert path, t1/t2 -> opt->diff_*() callbacks */
static void emit_diff(struct diff_options *opt, struct strbuf *path,
		      struct tree_desc *t1, struct tree_desc *t2)
{
	unsigned int mode1 = t1 ? t1->entry.mode : 0;
	unsigned int mode2 = t2 ? t2->entry.mode : 0;

	if (mode1 && mode2) {
		opt->change(opt, mode1, mode2, t1->entry.sha1, t2->entry.sha1,
			1, 1, path->buf, 0, 0);
	}
	else {
		const unsigned char *sha1;
		unsigned int mode;
		int addremove;

		if (mode2) {
			addremove = '+';
			sha1 = t2->entry.sha1;
			mode = mode2;
		} else {
			addremove = '-';
			sha1 = t1->entry.sha1;
			mode = mode1;
		}

		opt->add_remove(opt, addremove, mode, sha1, 1, path->buf, 0);
	}
}


/* new path should be added to diff
 *
 * 3 cases on how/when it should be called and behaves:
 *
 *	!t1,  t2	-> path added, parent lacks it
 *	 t1, !t2	-> path removed from parent
 *	 t1,  t2	-> path modified
 */
static void show_path(struct strbuf *base, struct diff_options *opt,
		      struct tree_desc *t1, struct tree_desc *t2)
{
	unsigned mode;
	const char *path;
	int pathlen;
	int old_baselen = base->len;
	int isdir, recurse = 0, emitthis = 1;

	/* at least something has to be valid */
	assert(t1 || t2);

	if (t2) {
		/* path present in resulting tree */
		tree_entry_extract(t2, &path, &mode);
		pathlen = tree_entry_len(&t2->entry);
		isdir = S_ISDIR(mode);
	} else {
		/*
		 * a path was removed - take path from parent. Also take
		 * mode from parent, to decide on recursion.
		 */
		tree_entry_extract(t1, &path, &mode);
		pathlen = tree_entry_len(&t1->entry);

		isdir = S_ISDIR(mode);
		mode = 0;
	}

	if (DIFF_OPT_TST(opt, RECURSIVE) && isdir) {
		recurse = 1;
		emitthis = DIFF_OPT_TST(opt, TREE_IN_RECURSIVE);
	}

	strbuf_add(base, path, pathlen);

	if (emitthis)
		emit_diff(opt, base, t1, t2);

	if (recurse) {
		strbuf_addch(base, '/');
		ll_diff_tree_sha1(t1 ? t1->entry.sha1 : NULL,
				  t2 ? t2->entry.sha1 : NULL, base, opt);
	}

	strbuf_setlen(base, old_baselen);
}

static void skip_uninteresting(struct tree_desc *t, struct strbuf *base,
			       struct diff_options *opt)
{
	enum interesting match;

	while (t->size) {
		match = tree_entry_interesting(&t->entry, base, 0, &opt->pathspec);
		if (match) {
			if (match == all_entries_not_interesting)
				t->size = 0;
			break;
		}
		update_tree_entry(t);
	}
}

static int ll_diff_tree_sha1(const unsigned char *old, const unsigned char *new,
			     struct strbuf *base, struct diff_options *opt)
{
	struct tree_desc t1, t2;
	void *t1tree, *t2tree;

	t1tree = fill_tree_descriptor(&t1, old);
	t2tree = fill_tree_descriptor(&t2, new);

	/* Enable recursion indefinitely */
	opt->pathspec.recursive = DIFF_OPT_TST(opt, RECURSIVE);

	for (;;) {
		int cmp;

		if (diff_can_quit_early(opt))
			break;
		if (opt->pathspec.nr) {
			skip_uninteresting(&t1, base, opt);
			skip_uninteresting(&t2, base, opt);
		}
		if (!t1.size && !t2.size)
			break;

		cmp = tree_entry_pathcmp(&t1, &t2);

		/* t1 = t2 */
		if (cmp == 0) {
			if (DIFF_OPT_TST(opt, FIND_COPIES_HARDER) ||
			    hashcmp(t1.entry.sha1, t2.entry.sha1) ||
			    (t1.entry.mode != t2.entry.mode))
				show_path(base, opt, &t1, &t2);

			update_tree_entry(&t1);
			update_tree_entry(&t2);
		}

		/* t1 < t2 */
		else if (cmp < 0) {
			show_path(base, opt, &t1, /*t2=*/NULL);
			update_tree_entry(&t1);
		}

		/* t1 > t2 */
		else {
			show_path(base, opt, /*t1=*/NULL, &t2);
			update_tree_entry(&t2);
		}
	}

	free(t2tree);
	free(t1tree);
	return 0;
}

/*
 * Does it look like the resulting diff might be due to a rename?
 *  - single entry
 *  - not a valid previous file
 */
static inline int diff_might_be_rename(void)
{
	return diff_queued_diff.nr == 1 &&
		!DIFF_FILE_VALID(diff_queued_diff.queue[0]->one);
}

static void try_to_follow_renames(const unsigned char *old, const unsigned char *new, struct strbuf *base, struct diff_options *opt)
{
	struct diff_options diff_opts;
	struct diff_queue_struct *q = &diff_queued_diff;
	struct diff_filepair *choice;
	int i;

	/*
	 * follow-rename code is very specific, we need exactly one
	 * path. Magic that matches more than one path is not
	 * supported.
	 */
	GUARD_PATHSPEC(&opt->pathspec, PATHSPEC_FROMTOP | PATHSPEC_LITERAL);
#if 0
	/*
	 * We should reject wildcards as well. Unfortunately we
	 * haven't got a reliable way to detect that 'foo\*bar' in
	 * fact has no wildcards. nowildcard_len is merely a hint for
	 * optimization. Let it slip for now until wildmatch is taught
	 * about dry-run mode and returns wildcard info.
	 */
	if (opt->pathspec.has_wildcard)
		die("BUG:%s:%d: wildcards are not supported",
		    __FILE__, __LINE__);
#endif

	/* Remove the file creation entry from the diff queue, and remember it */
	choice = q->queue[0];
	q->nr = 0;

	diff_setup(&diff_opts);
	DIFF_OPT_SET(&diff_opts, RECURSIVE);
	DIFF_OPT_SET(&diff_opts, FIND_COPIES_HARDER);
	diff_opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_opts.single_follow = opt->pathspec.items[0].match;
	diff_opts.break_opt = opt->break_opt;
	diff_opts.rename_score = opt->rename_score;
	diff_setup_done(&diff_opts);
	ll_diff_tree_sha1(old, new, base, &diff_opts);
	diffcore_std(&diff_opts);
	free_pathspec(&diff_opts.pathspec);

	/* Go through the new set of filepairing, and see if we find a more interesting one */
	opt->found_follow = 0;
	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];

		/*
		 * Found a source? Not only do we use that for the new
		 * diff_queued_diff, we will also use that as the path in
		 * the future!
		 */
		if ((p->status == 'R' || p->status == 'C') &&
		    !strcmp(p->two->path, opt->pathspec.items[0].match)) {
			const char *path[2];

			/* Switch the file-pairs around */
			q->queue[i] = choice;
			choice = p;

			/* Update the path we use from now on.. */
			path[0] = p->one->path;
			path[1] = NULL;
			free_pathspec(&opt->pathspec);
			parse_pathspec(&opt->pathspec,
				       PATHSPEC_ALL_MAGIC & ~PATHSPEC_LITERAL,
				       PATHSPEC_LITERAL_PATH, "", path);

			/*
			 * The caller expects us to return a set of vanilla
			 * filepairs to let a later call to diffcore_std()
			 * it makes to sort the renames out (among other
			 * things), but we already have found renames
			 * ourselves; signal diffcore_std() not to muck with
			 * rename information.
			 */
			opt->found_follow = 1;
			break;
		}
	}

	/*
	 * Then, discard all the non-relevant file pairs...
	 */
	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		diff_free_filepair(p);
	}

	/*
	 * .. and re-instate the one we want (which might be either the
	 * original one, or the rename/copy we found)
	 */
	q->queue[0] = choice;
	q->nr = 1;
}

int diff_tree_sha1(const unsigned char *old, const unsigned char *new, const char *base_str, struct diff_options *opt)
{
	struct strbuf base;
	int retval;

	strbuf_init(&base, PATH_MAX);
	strbuf_addstr(&base, base_str);

	retval = ll_diff_tree_sha1(old, new, &base, opt);
	if (!*base_str && DIFF_OPT_TST(opt, FOLLOW_RENAMES) && diff_might_be_rename())
		try_to_follow_renames(old, new, &base, opt);

	strbuf_release(&base);

	return retval;
}

int diff_root_tree_sha1(const unsigned char *new, const char *base, struct diff_options *opt)
{
	return diff_tree_sha1(NULL, new, base, opt);
}
