PYTHON := python3

.PHONY: all install test clean asan test-asan

all: install

install:
	$(PYTHON) -m pip install -e .

test: install
	$(PYTHON) -m pytest tests/ -v

# --- ASAN build and test ---
# Builds the C extension with AddressSanitizer and runs the test suite.
# ASAN is preloaded so the Python interpreter picks it up.

ASAN_FLAGS := -O1 -g -fsanitize=address -fno-omit-frame-pointer

asan:
	CFLAGS="$(ASAN_FLAGS)" \
	LDSHARED="cc -shared -fsanitize=address" \
	$(PYTHON) -m pip install -e . --no-build-isolation

test-asan: asan
	@ASAN_LIB=$$($(PYTHON) -c "import subprocess; r=subprocess.run(['cc','-print-file-name=libasan.so'],capture_output=True,text=True); print(r.stdout.strip())") && \
	echo "Using ASAN lib: $$ASAN_LIB" && \
	LD_PRELOAD=$$ASAN_LIB \
	ASAN_OPTIONS=detect_leaks=0 \
	$(PYTHON) -m pytest tests/ -v -k "not rollover"

clean:
	rm -rf build/ dist/ *.egg-info src/*.egg-info
	find . -name '*.so' -delete
	find . -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
