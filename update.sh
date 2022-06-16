#!/bin/sh

pull() {
	ret=1

	branch="$(git branch --show-current)"

	if git stash -q; then
		if git checkout -q master; then
			git pull && ret=0

			git checkout -q "$branch"
		fi

		git stash pop -q
	fi

	return "$ret"
}

commit() {
	git add index.html &&
		git commit --no-edit --message='Update man page'
}

pull &&
	git show master:doc/ctpv.1 | ./manhtml.sh > index.html &&
	commit
