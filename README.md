# cpufreq

`cpufreq` is a small Windows command-line utility that limits the maximum CPU
frequency used by the active power scheme. It can apply a limit once or remain
running so that the limit is reapplied after the computer resumes from sleep or
hibernation and whenever the active power scheme changes.

By default, the utility limits both AC (plugged-in) and DC (battery) operation
to **800 MHz**.

## Requirements

- Windows 8 or later
- Permission to modify Windows power settings (run from an elevated terminal if
  Windows reports an access-denied error)
- One of the following C toolchains to build from source:
  - Microsoft Visual C/C++ (MSVC)
  - GCC in an MSYS2 UCRT64 environment

## Build

### MSVC

Open a Visual Studio Developer Command Prompt and run:

```bat
nmake /f Makefile.msvc
```

### MinGW-w64

Open an MSYS2 UCRT64 shell and run:

```sh
mingw32-make -f Makefile.mingw
```

Both builds produce `cpufreq.exe` in the repository root. To remove generated
files, use the corresponding makefile's `clean` target:

```bat
nmake /f Makefile.msvc clean
```

or:

```sh
mingw32-make -f Makefile.mingw clean
```

## Usage

```text
cpufreq.exe [options]
```

Run `cpufreq.exe` without a mode option to apply the requested limits and keep
the program resident. Stop it with <kbd>Ctrl</kbd>+<kbd>C</kbd> or by closing its
console window.

### Frequency options

| Option | Description |
| --- | --- |
| `--mhz <MHz>` | Set both AC and DC maximum frequencies. |
| `--ac-mhz <MHz>` | Set the maximum frequency while plugged in. |
| `--dc-mhz <MHz>` | Set the maximum frequency while on battery. |

Valid frequency values are `100` through `64000` MHz. The special value `0`
means unlimited. If `--mhz` is combined with `--ac-mhz` or `--dc-mhz`, the
individual AC or DC value takes precedence regardless of argument order.

### Mode options

| Option | Description |
| --- | --- |
| `--once` | Apply the selected limits and exit. |
| `--unthrottle-once` | Set both limits to unlimited and exit. |
| `--release-once` | Alias for `--unthrottle-once`. |
| `-h`, `--help` | Display command-line help. |

### Examples

Keep a 1.2 GHz limit active for both AC and battery operation:

```powershell
.\cpufreq.exe --mhz 1200
```

Use different plugged-in and battery limits:

```powershell
.\cpufreq.exe --ac-mhz 2400 --dc-mhz 800
```

Apply a limit once without leaving the utility running:

```powershell
.\cpufreq.exe --mhz 1800 --once
```

Remove the limits written by the utility:

```powershell
.\cpufreq.exe --unthrottle-once
```

## How it works

The utility updates the `PROCFREQMAX`, `PROCFREQMAX1`, and `PROCFREQMAX2`
processor power settings for the currently active Windows power scheme, for
both AC and DC power, and then reactivates that scheme so the changes take
effect.

In resident mode it listens for suspend/resume and active-power-scheme
notifications. Resume events are debounced for five seconds to avoid applying
the same settings twice when Windows emits multiple notifications in quick
succession.

The values are stored in the Windows power scheme; stopping the resident
process does **not** restore the previous values. Use `--unthrottle-once` to set
the affected values to Windows' unlimited/default value (`0`).

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | The operation succeeded, or help was displayed. |
| `1` | A Windows power or notification API operation failed. |
| `2` | The command line was invalid. |

## License

This project is licensed under the [MIT License](LICENSE).
