# Quick Start: Compile with Visual Studio

**TL;DR** - Follow these steps to compile with Visual Studio in ~15 minutes.

## Step 1: Install vcpkg (5 minutes)

Open **PowerShell** and run:

```powershell
cd C:\
mkdir dev -ErrorAction SilentlyContinue
cd dev
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
```

## Step 2: Install Libraries (10 minutes)

Still in the vcpkg directory:

```powershell
.\vcpkg install libwebsockets:x64-windows openssl:x64-windows
```

☕ This takes 5-15 minutes. Go grab a coffee!

## Step 3: Open Developer Command Prompt

1. Press `Win` key
2. Search for **"Developer Command Prompt for VS"** or **"x64 Native Tools"**
3. Right-click → **Run as Administrator**
4. Navigate to project:
   ```cmd
   cd /d E:\brainboost\brainboost_projects\subjective_kvm\cinput
   ```

## Step 4: Compile

```powershell
.\compile_vs.ps1
```

When prompted about copying DLLs, press **Y**.

## Step 5: Test

```powershell
.\input_unified.exe --help
```

You should see the help text!

---

## If You Installed vcpkg Somewhere Else

If you installed vcpkg in a different location (not `C:\dev\vcpkg`), specify the path:

```powershell
.\compile_vs.ps1 -VcpkgPath "D:\my\vcpkg"
```

---

## Troubleshooting

### "cl.exe not found"
- You're not in the Developer Command Prompt
- Search for "Developer Command Prompt for VS" in Start menu

### "vcpkg installation not found"
- You didn't install vcpkg at `C:\dev\vcpkg`
- Use: `.\compile_vs.ps1 -VcpkgPath "YOUR\PATH"`

### "websockets.dll is missing"
- Run: `.\compile_vs.ps1` again and press Y to copy DLLs
- Or manually: `Copy-Item "C:\dev\vcpkg\installed\x64-windows\bin\*.dll" -Destination .`

---

## Alternative: Use MinGW Instead

If Visual Studio is giving you trouble, MinGW is much simpler:

1. See: `COMPILE_INSTRUCTIONS_WINDOWS.md`
2. Or run: `.\install_msys2_and_compile.ps1` from the parent directory

The compiled binary works identically regardless of compiler!

---

## Full Guide

For detailed explanations, troubleshooting, and Visual Studio IDE setup, see:
- `VISUAL_STUDIO_COMPILE_GUIDE.md`







