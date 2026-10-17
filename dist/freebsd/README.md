# FreeBSD ports for BASound

This directory contains two ready-to-import ports:

```
audio/
├── basound/          kernel modules
│   ├── Makefile
│   ├── distinfo
│   ├── pkg-descr
│   ├── pkg-plist
│   └── files/
│       └── pkg-message.in
└── basound-tools/    FLTK mixer/control utilities
    ├── Makefile
    ├── pkg-descr
    └── distinfo
```

`audio/basound` installs the kernel modules into `/boot/modules`.
`audio/basound-tools` installs the four mixer programs into
`${PREFIX}/bin` and depends on FLTK and X11.

The tools are a separate port so that installing a kernel module does
not pull in a full X11/FLTK dependency chain, and so that the tools can
be installed on a machine that builds the modules locally.

## Importing into the ports tree

From the top of a `freebsd/freebsd-ports` checkout:

```
cp -R dist/freebsd/audio/basound /usr/ports/audio/
cp -R dist/freebsd/audio/basound-tools /usr/ports/audio/

cd /usr/ports/audio/basound && portlint -A && make stage check-plist
cd /usr/ports/audio/basound-tools && portlint -A && make stage check-plist
```

## Package behavior

`audio/basound` uses `USES=kmod`, which:

- installs the modules into `${KMODDIR}` (default `/boot/modules`),
- requires kernel sources in `${SRC_BASE}` at build time,
- appends the `_OSVERSION` suffix to the package version, and
- adds the `@kld` plist entry so `kldxref` runs on install.

`audio/basound-tools` builds the per-device Makefiles under `tools/`
with `gmake` and installs:

```
bsm_hdsp       RME Hammerfall DSP matrix mixer  (/dev/hdspN)
bsm_digi00x    Digi 002/003 mixer               (/dev/dspN)
bsm_line6      Line6 controls and meters        (/dev/dspN)
bsm_pf2626     M-Audio ProFire 2626 mixer       (/dev/pf2626N)
```

## Updating the ports

Both ports track the same upstream snapshot:

```
DISTVERSION=	0.1
GH_TAGNAME=	f5d5389d77c15aca54e95b344c0c4031c4a12afe
```

When a release is tagged (or a newer commit is chosen), update both
`Makefile`s, then regenerate the checksums in each port:

```
make makesum
make clean
make stage check-plist stage-qa
```

Note that the kmod port's `do-build` explicitly generates `pci_if.h`
because the pinned snapshot predates adding it to the top-level `SRCS`.
That line can be removed once `GH_TAGNAME` points at a commit that
includes the fix.

## Submitting

Ports are submitted as patches against the ports tree:

```
cd /usr/ports
git switch -c basound
git add audio/basound audio/basound-tools
git commit -m "audio/basound: new port"
git format-patch -1
```

Attach the patch to a Bugzilla PR in the `Ports & Packages` product,
component `Individual Port(s)`, or open a pull request against
`freebsd/freebsd-ports`.

## Notes

- Both ports use the same distfile (`GH_ACCOUNT=cvscpp`,
  `GH_PROJECT=basound`) and therefore the same checksums.
- Upstream is licensed under the GNU GPL v3 or later.
- The kernel module and the tools must be built against a matching
  source snapshot; `basound-tools` does not require kernel sources.
