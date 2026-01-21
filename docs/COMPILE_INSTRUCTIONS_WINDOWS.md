# Windows Compilation Instructions for input_unified.c

## Prerequisites

To compile the C version of the KVM client on Windows, you need:

1. **MSYS2** with MinGW-w64 toolchain
2. **libwebsockets** library
3. **OpenSSL** library

## Step 1: Install MSYS2

1. Download MSYS2 from: https://www.msys2.org/
2. Run the installer (e.g., `msys2-x86_64-xxxxxxxx.exe`)
3. Follow the installation wizard (default location: `C:\msys64`)
4. After installation, MSYS2 will launch automatically

## Step 2: Update MSYS2

In the MSYS2 terminal that opens, run:

```bash
pacman -Syu
```

If it prompts to close the terminal, close it and reopen "MSYS2 MSYS" from the Start menu, then run again:

```bash
pacman -Syu
```

## Step 3: Install Development Tools and Libraries

Close any MSYS2 terminals and open **"MSYS2 MinGW 64-bit"** from the Start menu.

Install the required packages:

```bash
pacman -S mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-make \
          mingw-w64-x86_64-libwebsockets \
          mingw-w64-x86_64-openssl
```

## Step 4: Add MinGW to Windows PATH

You have two options:

### Option A: Temporarily (for current PowerShell session only)

```powershell
$env:Path = "C:\msys64\mingw64\bin;" + $env:Path
```

### Option B: Permanently (recommended)

1. Press `Win + X` and select "System"
2. Click "Advanced system settings"
3. Click "Environment Variables"
4. Under "User variables" or "System variables", find "Path"
5. Click "Edit"
6. Click "New" and add: `C:\msys64\mingw64\bin`
7. Click "OK" on all dialogs
8. **Restart your terminal/PowerShell**

## Step 5: Verify Installation

Open a **new** PowerShell terminal and verify gcc is available:

```powershell
gcc --version
```

You should see output like:
```
gcc.exe (Rev10, Built by MSYS2 project) 13.x.x
```

## Step 6: Compile

Navigate to the `cinput` directory and run the compile script:

```powershell
cd E:\brainboost\brainboost_projects\subjective_kvm\cinput
.\compile.ps1
```

Or compile manually:

```powershell
gcc -Wall -Wextra -O2 -g -DWIN32 -o input_unified.exe input_unified.c -lwebsockets -lssl -lcrypto -lws2_32 -luser32
```

## Step 7: Run the Compiled Binary

After successful compilation:

```powershell
.\input_unified.exe --help
```

## Troubleshooting

### "gcc not found"
- Make sure you added `C:\msys64\mingw64\bin` to your PATH
- Restart your terminal after adding to PATH
- Verify with: `where.exe gcc`

### Linker errors (undefined reference to libwebsockets or OpenSSL)
- Make sure you installed the libraries: `pacman -S mingw-w64-x86_64-libwebsockets mingw-w64-x86_64-openssl`
- Open **MSYS2 MinGW 64-bit** terminal (not MSYS2 MSYS or UCRT64)

### "cannot find -lwebsockets"
- The libraries might be installed in the wrong MSYS2 subsystem
- Use MSYS2 MinGW 64-bit terminal and reinstall the packages

## Alternative: Using MSYS2 Terminal Directly

If you prefer, you can compile directly in the MSYS2 MinGW 64-bit terminal:

```bash
cd /e/brainboost/brainboost_projects/subjective_kvm/cinput
gcc -Wall -Wextra -O2 -g -DWIN32 -o input_unified.exe input_unified.c -lwebsockets -lssl -lcrypto -lws2_32 -luser32
```

## Next Steps

Once compiled successfully, refer to the main documentation for running the KVM client:
- `WINDOWS_QUICK_START.md`
- `README.md`

