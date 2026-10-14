# TOS ROM images

Put your own TOS dumps here (they are copyrighted and not part of this
repository):

| File | TOS | Size |
|---|---|---|
| `tos162.img` | 1.62 | 256 KB |
| `tos206.img` | 2.06 | 256 KB |

`idf.py flash` writes them to the `tos162` and `tos206` flash partitions.
Either one is enough, but then choose that TOS on the setup screen: it
always offers both, and the ST won't start from a missing image.
