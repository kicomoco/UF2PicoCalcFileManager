#!/usr/bin/env python3
"""Regenerate src/ca_certs.h from a PEM bundle.

Usage: tools/gen_ca_certs.py roots.pem > src/ca_certs.h
"""
import sys

pem = open(sys.argv[1]).read()
print("/* Root CA certificates used to verify GitHub TLS connections. */")
print("#pragma once\n")
print("static const char GITHUB_CA_CERTS[] =")
lines = pem.strip().splitlines()
for i, line in enumerate(lines):
    end = ";" if i == len(lines) - 1 else ""
    print('    "%s\\n"%s' % (line, end))
