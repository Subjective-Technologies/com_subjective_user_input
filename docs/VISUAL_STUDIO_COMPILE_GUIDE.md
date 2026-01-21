# Compiling with Visual Studio (MSVC)

This guide will help you compile `input_unified.c` using Visual Studio's compiler (cl.exe).

## Prerequisites

1. **Visual Studio** (you already have this installed)
   - Visual Studio 2019, 2022, or Visual Studio Build Tools
   - Make sure "Desktop development with C++" workload is installed

2. **vcpkg** - Microsoft's package manager for C/C++ libraries
   - We'll use this to install `libwebsockets` and `openssl`

---

## Step 1: Install vcpkg

Open PowerShell and run:

```powershell
# Navigate to a directory where you want vcpkg (e.g., C:\dev)
cd C:\
mkdir dev
cd dev

# Clone vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg

# Bootstrap vcpkg
.\bootstrap-vcpkg.bat
```

This will create `vcpkg.exe` in the vcpkg directory.

---

## Step 2: Install Dependencies

Still in the vcpkg directory, install the required libraries:

```powershell
# Install libwebsockets and openssl for x64-windows
.\vcpkg install libwebsockets:x64-windows openssl:x64-windows

# This will take 5-15 minutes depending on your machine
```

**Note**: If you need 32-bit (x86), use `:x86-windows` instead.

---

## Step 3: Integrate vcpkg with Visual Studio (Optional but Recommended)

This makes vcpkg libraries automatically available to all Visual Studio projects:

```powershell
.\vcpkg integrate install
```

You'll see a message like:
```
Applied user-wide integration for this vcpkg root.
All MSBuild C++ projects can now #include any installed libraries.
```

---

## Step 4: Open Visual Studio Developer Command Prompt

You need to compile in a special terminal that has Visual Studio tools available:

1. Press `Win` key and search for **"Developer Command Prompt for VS"** or **"x64 Native Tools Command Prompt for VS"**
2. Run it as Administrator (right-click → Run as Administrator)
3. Navigate to your project:
   ```cmd
   cd /d E:\brainboost\brainboost_projects\subjective_kvm\cinput
   ```

---

## Step 5: Compile with cl.exe

### Option A: Using the updated PowerShell script

From the Developer Command Prompt (PowerShell mode):

```powershell
.\compile_vs.ps1
```

### Option B: Manual compilation

```cmd
cl.exe /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /DWIN32 ^
    /I"C:\dev\vcpkg\installed\x64-windows\include" ^
    input_unified.c ^
    /link ^
    /LIBPATH:"C:\dev\vcpkg\installed\x64-windows\lib" ^
    websockets.lib libssl.lib libcrypto.lib ^
    ws2_32.lib user32.lib advapi32.lib crypt32.lib ^
    /OUT:input_unified.exe
```

**Adjust paths** if you installed vcpkg somewhere other than `C:\dev\vcpkg`.

---

## Step 6: Copy Required DLLs

The compiled executable needs DLL files at runtime. Copy them to the `cinput` directory:

```powershell
# From the cinput directory
Copy-Item "C:\dev\vcpkg\installed\x64-windows\bin\*.dll" -Destination . -Force
```

Or copy only the required ones:
- `websockets.dll`
- `libssl-*.dll`
- `libcrypto-*.dll`
- `zlib1.dll` (dependency of libwebsockets)

---

## Step 7: Test the Compiled Binary

```powershell
.\input_unified.exe --help
```

You should see the help text for the KVM client.

---

## Troubleshooting

### "cl.exe is not recognized"

- You're not in the Developer Command Prompt
- Search for "Developer Command Prompt for VS" or "x64 Native Tools Command Prompt"

### "Cannot open include file 'libwebsockets.h'"

- vcpkg libraries not found
- Make sure you pass the correct include path: `/I"C:\dev\vcpkg\installed\x64-windows\include"`
- Verify libwebsockets is installed: `C:\dev\vcpkg\vcpkg list`

### "Unresolved external symbol" errors

- Missing library in the link command
- Make sure you're linking: `websockets.lib libssl.lib libcrypto.lib ws2_32.lib user32.lib`

### "The program can't start because websockets.dll is missing"

- The DLL files need to be in the same directory as the executable
- Copy DLLs from `C:\dev\vcpkg\installed\x64-windows\bin\` to the `cinput` directory

### vcpkg is too slow or you don't have git

- You can download pre-built binaries of libwebsockets and OpenSSL from their official websites
- Or use the MSYS2 method from `COMPILE_INSTRUCTIONS_WINDOWS.md` (easier)

---

## Alternative: Using Visual Studio IDE (GUI)

If you prefer using the Visual Studio GUI:

1. Open Visual Studio
2. Create a new "Console App" C++ project (empty project)
3. Add `input_unified.c` to the project
4. Right-click project → Properties
5. Under **C/C++ → General → Additional Include Directories**, add:
   ```
   C:\dev\vcpkg\installed\x64-windows\include
   ```
6. Under **Linker → General → Additional Library Directories**, add:
   ```
   C:\dev\vcpkg\installed\x64-windows\lib
   ```
7. Under **Linker → Input → Additional Dependencies**, add:
   ```
   websockets.lib
   libssl.lib
   libcrypto.lib
   ws2_32.lib
   user32.lib
   advapi32.lib
   crypt32.lib
   ```
8. Under **C/C++ → Preprocessor → Preprocessor Definitions**, add:
   ```
   WIN32
   _CRT_SECURE_NO_WARNINGS
   ```
9. Build the project (F7)

---

## Recommendation

For simplicity, I recommend using **MSYS2/MinGW** (from `COMPILE_INSTRUCTIONS_WINDOWS.md`) unless you specifically need Visual Studio compilation. MinGW is much easier to set up and the compiled binary will work identically.

However, if you need Visual Studio for debugging or integration with other MSVC projects, the vcpkg method above is the cleanest approach.

---

## Next Steps

Once compiled, see:
- `WINDOWS_QUICK_START.md` - for usage instructions
- `README.md` - for general KVM client documentation







