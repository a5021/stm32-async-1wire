# Security Policy

## Supported Versions

Only the latest release is actively supported with security fixes.

| Version | Supported          |
|---------|--------------------|
| latest  | :white_check_mark: |
| older   | :x:                |

## Reporting a Vulnerability

This project is a bare-metal firmware demo/driver for the STM32F103C8T6
and does not process untrusted input in a typical networked sense.
However, if you find a security issue — for example in the 1-Wire
protocol handling, buffer handling in `src/ds18b20.c` or
`src/demo.c`, or in the build tooling — please do **not** open a
public issue.

Instead, report it privately by opening an issue on GitHub and marking
it as a security vulnerability (GitHub's "Report a vulnerability"
option), or contact the maintainer directly.

Please include:

- A description of the vulnerability
- Steps to reproduce
- The affected version(s)
- Any suggested fix, if you have one

We aim to acknowledge reports within a few business days and to publish
a fix in a new release as soon as practical.
