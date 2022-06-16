#!/bin/sh

pull() {
	ret=0

	branch="$(git branch --show-current)"

	git stash

	git checkout master && git pull || ret=1

	git checkout "$branch"
	git stash pop

	return "$ret"
}

commit() {
	git add index.html
	git commit --no-edit --message='Update man page'
}

pull &&
	git show master:doc/ctpv.1 | ./manhtml.sh > index.html &&
	commit
