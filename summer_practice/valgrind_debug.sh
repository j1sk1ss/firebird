#!bin/bash
sudo valgrind --leak-check=full --show-leak-kinds=all --trace-children=yes --track-origins=yes --log-file=firebird_valgrind.log ./firebird
# [PRACTICE_MEMLEAK <date>]
# /usr/local/firebird/bin
