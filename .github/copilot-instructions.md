# Rules for AI Assistant

1. **NEVER** change platform, library, or framework versions in `platformio.ini`, `package.json`, or `idf_component.yml` without EXPLICIT user permission.
2. **NEVER** delete files without asking.
3. If a build fails, analyze and report the cause, but **DO NOT** attempt to fix configuration files automatically.

## ESP-IDF + Arduino Framework Header Pattern

**Problem**: `portYIELD_CORE()` missing definition error when using ESP-IDF + Arduino as component.

**Root Cause**: Header mismatch - mixing FreeRTOS headers from Arduino framework package with ESP-IDF framework package.

**Solution**:
- Include `<Arduino.h>` in header files that need FreeRTOS types (`portMUX_TYPE`, `portENTER_CRITICAL`, etc.)
- Remove direct includes of `freertos/portmacro.h` or `freertos/FreeRTOS.h`
- Let Arduino.h provide all FreeRTOS headers consistently
- This ensures all files use the same FreeRTOS header source, preventing port/config mismatches

## Code Quality and Maintenance

4. **Code Refactoring Process**:
   - Periodically analyze files for opportunities to split them into smaller, more maintainable modules
   - Present a detailed analysis and proposal before starting any refactoring work
   - Wait for explicit approval before proceeding with the refactoring

5. **Dead Code Cleanup**:
   - Regularly identify unused code, deprecated functions, or outdated implementations
   - **ALWAYS ASK** before removing any code, even if clearly marked as deprecated
   - Provide context about what the code does and why it appears unused

6. **Best Practices Research**:
   - Periodically research current best practices for specific code sections using available resources
   - Present findings and suggest improvements before implementing changes
   - Wait for approval before refactoring based on best practices
