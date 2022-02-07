#!/bin/bash

echo Command line: $@

echo $@ > /tmp/cmdline.txt

echo Press ENTER to continue

read dummy

exit 0
