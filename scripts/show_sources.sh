#!/bin/bash
objdump -Wi $@ | grep -oP 'DW_AT_name.*\.cpp'
