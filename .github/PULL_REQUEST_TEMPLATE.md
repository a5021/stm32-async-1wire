## Description

<!-- What does this pull request change, and why? -->

## Related issue

<!-- Link to the issue this PR resolves, if any (e.g. #12). -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Build system / tooling
- [ ] Documentation
- [ ] Other (please describe)

## Design goals check

- [ ] The driver remains non-blocking (no `delay_us()` / busy-wait)
- [ ] The driver remains interrupt-free (no NVIC usage)
- [ ] Register-level access only — no HAL/LL introduced
- [ ] Code follows `.clang-format` style

## Verification

- [ ] `make` builds cleanly (release, `-Werror`)
- [ ] `make SYSCLK_MHZ=8` builds cleanly
- [ ] `make test` passes (and `-f0` / `-g0` / `-f4` if a backend was touched)
- [ ] `make test-clocks` passes (per-family clock defaults)
- [ ] `make test-chips` passes (after adding or editing a `chips/<part>.mk`)
- [ ] `clang-format --dry-run --Werror` passes on changed files
- [ ] `cppcheck` passes
- [ ] README and CHANGELOG updated if behavior/usage changed

## Hardware testing

<!-- Describe what was tested and on what hardware (optional but appreciated). -->
