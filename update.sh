#!/bin/sh

git show master:doc/ctpv.1 | ./manhtml.sh > index.html
