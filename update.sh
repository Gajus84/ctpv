#!/bin/sh

branch="$(git branch --show-current)"

git stash
git checkout master
git pull
git checkout "$branch"
git stash pop

git show master:doc/ctpv.1 | ./manhtml.sh > index.html
