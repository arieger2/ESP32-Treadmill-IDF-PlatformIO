"""
PlatformIO Pre-Build Script
Compiles TypeScript before building firmware
"""

Import("env")
import subprocess
import os
import sys

def compile_typescript(source, target, env):
    """Compile TypeScript to JavaScript and minify"""
    print("=" * 60)
    print("Building TypeScript UI modules...")
    print("=" * 60)
    
    project_dir = env.get("PROJECT_DIR")
    os.chdir(project_dir)
    
    # Check if node_modules exists
    if not os.path.exists("node_modules"):
        print("Installing npm dependencies...")
        result = subprocess.run(["npm", "install"], capture_output=True, text=True)
        if result.returncode != 0:
            print("ERROR: npm install failed!")
            print(result.stderr)
            sys.exit(1)
    
    # Run TypeScript build
    print("Compiling TypeScript and minifying...")
    result = subprocess.run(["npm", "run", "build"], capture_output=True, text=True)
    
    if result.returncode != 0:
        print("ERROR: TypeScript compilation failed!")
        print(result.stdout)
        print(result.stderr)
        sys.exit(1)
    
    # Check workout bundle
    if not os.path.exists("build/workout-app.min.js"):
        print("ERROR: build/workout-app.min.js not found!")
        sys.exit(1)
    workout_size = os.path.getsize("build/workout-app.min.js")
    print(f"✓ Workout: build/workout-app.min.js ({workout_size} bytes)")
    
    # Check monitor bundle
    if not os.path.exists("build/monitor-app.min.js"):
        print("ERROR: build/monitor-app.min.js not found!")
        sys.exit(1)
    monitor_size = os.path.getsize("build/monitor-app.min.js")
    print(f"✓ Monitor: build/monitor-app.min.js ({monitor_size} bytes)")
    
    # Check settings bundle
    if not os.path.exists("build/settings-app.min.js"):
        print("ERROR: build/settings-app.min.js not found!")
        sys.exit(1)
    settings_size = os.path.getsize("build/settings-app.min.js")
    print(f"✓ Settings: build/settings-app.min.js ({settings_size} bytes)")
    
    # Convert workout to C header with raw string literal (null-terminated)
    print("Converting to C headers...")
    result = subprocess.run([
        "sh", "-c",
        "printf 'static const char PROGMEM WORKOUT_TS_BUNDLE[] = R\"JS(' > build/workout-app.min.js.h && " +
        "cat build/workout-app.min.js >> build/workout-app.min.js.h && " +
        "printf ')JS\";\\n' >> build/workout-app.min.js.h"
    ], capture_output=True, text=True)
    
    if result.returncode != 0 or not os.path.exists("build/workout-app.min.js.h"):
        print("ERROR: Failed to create workout C header!")
        sys.exit(1)
    print("✓ Created: build/workout-app.min.js.h")
    
    # Convert monitor to C header with raw string literal (null-terminated)
    result = subprocess.run([
        "sh", "-c",
        "printf 'static const char PROGMEM MONITOR_TS_BUNDLE[] = R\"JS(' > build/monitor-app.min.js.h && " +
        "cat build/monitor-app.min.js >> build/monitor-app.min.js.h && " +
        "printf ')JS\";\\n' >> build/monitor-app.min.js.h"
    ], capture_output=True, text=True)
    
    if result.returncode != 0 or not os.path.exists("build/monitor-app.min.js.h"):
        print("ERROR: Failed to create monitor C header!")
        sys.exit(1)
    print("✓ Created: build/monitor-app.min.js.h")
    
    # Convert settings to C header with raw string literal (null-terminated)
    result = subprocess.run([
        "sh", "-c",
        "printf 'static const char PROGMEM SETTINGS_TS_BUNDLE[] = R\"JS(' > build/settings-app.min.js.h && " +
        "cat build/settings-app.min.js >> build/settings-app.min.js.h && " +
        "printf ')JS\";\\n' >> build/settings-app.min.js.h"
    ], capture_output=True, text=True)
    
    if result.returncode != 0 or not os.path.exists("build/settings-app.min.js.h"):
        print("ERROR: Failed to create settings C header!")
        sys.exit(1)
    print("✓ Created: build/settings-app.min.js.h")
    print("=" * 60)

env.AddPreAction("buildprog", compile_typescript)
