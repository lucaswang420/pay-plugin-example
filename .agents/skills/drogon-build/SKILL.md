---
name: drogon-build
description: Build Drogon payment plugin with correct Release mode configuration
user-invocable: false
---

## Build Requirements

CRITICAL: This project MUST be built in Release mode due to local Drogon framework configuration.

## Correct Build Commands

### Windows (from PayBackend/):
```bash
..\scripts\build.bat
```

### Never use:
- `cmake --build build` directly (wrong configuration)
- Debug mode builds (linker errors with Release Drogon)
- Visual Studio build button (unless configured for Release)

## Why This Matters

- Local Drogon is compiled in Release mode
- Mixing Debug/Release causes linker errors
- Conan manages all third-party dependencies
- build.bat handles all necessary CMake configuration

## Build Process Details

The build.bat script automatically:
1. Changes to PayBackend directory
2. Creates build directory if needed
3. Runs CMake with Release configuration
4. Sets up Conan toolchain
5. Builds the target in Release mode
6. Places output in build/Release/

## Post-Build Verification

After building, verify:
```bash
# Check executables exist
ls -lh build/Release/*.exe

# Run test suite
./build/Release/test_payplugin.exe

# Verify main executable
./build/Release/PayServer.exe --version
```

## Troubleshooting

### Linker errors with Drogon
**Problem**: Unresolved external symbols
**Solution**: Ensure Release mode build (Debug/Release mismatch)

### Conan dependency issues
**Problem**: Cannot find Conan packages
**Solution**: Run `conan install .` from PayBackend/ first

### Build速度快 but runtime crashes
**Problem**: Debug build succeeded but app crashes
**Solution**: You likely built Debug against Release Drogon - rebuild in Release mode

## Related Files

- Build script: `scripts/build.bat`
- CMake config: `PayBackend/CMakeLists.txt`
- Conan dependencies: `PayBackend/conanfile.txt`
- Project docs: `AGENTS.md` (Build Rules section)
