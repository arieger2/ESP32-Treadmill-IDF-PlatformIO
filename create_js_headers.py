#!/usr/bin/env python3
"""Create C header files from minified JavaScript files"""

import os

def create_header(js_file, header_file, var_name):
    """Convert JS file to C header with raw string literal"""
    if not os.path.exists(js_file):
        print(f"ERROR: {js_file} not found!")
        return False
    
    with open(js_file, 'r') as f:
        js_content = f.read()
    
    # Use raw string literal R"JS(...)JS" for proper embedding
    header_content = f'static const char PROGMEM {var_name}[] = R"JS({js_content})JS";\n'
    
    with open(header_file, 'w') as f:
        f.write(header_content)
    
    print(f"✓ Created: {header_file} ({len(js_content)} bytes)")
    return True

if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    
    print("Creating JavaScript C headers...")
    
    success = True
    success &= create_header("build/workout-app.min.js", "build/workout-app.min.js.h", "WORKOUT_TS_BUNDLE")
    success &= create_header("build/monitor-app.min.js", "build/monitor-app.min.js.h", "MONITOR_TS_BUNDLE")
    success &= create_header("build/settings-app.min.js", "build/settings-app.min.js.h", "SETTINGS_TS_BUNDLE")
    
    if success:
        print("✓ All headers created successfully!")
    else:
        print("ERROR: Some headers failed!")
        exit(1)
