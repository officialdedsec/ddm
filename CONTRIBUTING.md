# Contributing to DDM

Thank you for helping. DDM is free software under the [GPL-3.0](LICENSE); what you contribute is published under
the same licence.

## Reporting a problem
[Open an issue](https://github.com/officialdedsec/ddm/issues/new/choose) with DDM's version (**Help → About**), your
Windows version, what you did and what happened. For a video that doesn't download, the page's address is the most
useful thing you can give. **Security problems**: not in public, please — see [SECURITY.md](SECURITY.md).

## Changing the code
1. [BUILDING.md](BUILDING.md) explains how to build DDM and where things are.
2. Keep a change to one subject, and say in the pull request what it does and how you tried it.
3. DDM has to keep working on Windows 7 to 11, 32-bit: no API newer than Windows 7 without a fallback, and no new
   runtime or framework.
4. Texts people read are plain, short English; add new ones to `tools/strings-extra.txt` (at the end), and the
   translations you can to `tools/tr/`.
5. A site's videos stopped downloading? Often `installer/sites.json` (the rules every DDM picks up by itself) or
   `src/core/sites.cpp` is the place.

## Never commit
Keys, certificates or passwords (`*.pem`, `*.pfx`, `*.p12`, `*.key`), built programs, or the libraries' sources.
`.gitignore` already keeps them out.
