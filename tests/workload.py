"""Nontrivial workload for ground-truth trace comparison.

This module exercises a mix of Python functions, C builtins, class methods,
recursion, generators, and exception handling — representative of real-world
tracing targets.
"""


def fibonacci(n):
    """Recursive fibonacci — generates a deep call stack."""
    if n <= 1:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)


class DataProcessor:
    """Class with methods that exercise C builtins heavily."""

    def __init__(self, data):
        self.data = list(data)
        self.results = {}

    def process(self):
        self.sort_data()
        self.aggregate()
        self.filter_outliers()
        return self.results

    def sort_data(self):
        self.data.sort()

    def aggregate(self):
        total = sum(self.data)
        count = len(self.data)
        self.results["mean"] = total / count if count else 0
        self.results["min"] = min(self.data) if self.data else 0
        self.results["max"] = max(self.data) if self.data else 0

    def filter_outliers(self):
        mean = self.results.get("mean", 0)
        filtered = []
        for x in self.data:
            if abs(x - mean) < mean * 2:
                filtered.append(x)
        self.results["filtered_count"] = len(filtered)


def build_index(words):
    """Build a word index using dicts and string operations."""
    index = {}
    for i, word in enumerate(words):
        word = word.strip().lower()
        if word not in index:
            index[word] = []
        index[word].append(i)
    return index


def merge_sorted(a, b):
    """Merge two sorted lists — exercises list operations."""
    result = []
    i = j = 0
    while i < len(a) and j < len(b):
        if a[i] <= b[j]:
            result.append(a[i])
            i += 1
        else:
            result.append(b[j])
            j += 1
    result.extend(a[i:])
    result.extend(b[j:])
    return result


def generate_values(n):
    """Generator function."""
    for i in range(n):
        yield i * i


def safe_divide(a, b):
    """Exercises exception handling."""
    try:
        return a / b
    except ZeroDivisionError:
        return 0


def run_workload():
    """Main workload entry point."""
    # Recursion
    fib_result = fibonacci(10)

    # Class methods + C builtins
    import random
    random.seed(42)
    data = [random.randint(0, 100) for _ in range(200)]
    processor = DataProcessor(data)
    proc_result = processor.process()

    # String operations + dict building
    words = ["Hello", " World ", "hello", "WORLD", "test", "Test", "TEST"] * 30
    index = build_index(words)

    # Merge sorted lists
    a = sorted(random.sample(range(1000), 50))
    b = sorted(random.sample(range(1000), 50))
    merged = merge_sorted(a, b)

    # Generator consumption
    values = list(generate_values(100))

    # Exception handling
    results = [safe_divide(x, x - 50) for x in range(100)]

    return {
        "fib": fib_result,
        "proc": proc_result,
        "index_keys": len(index),
        "merged_len": len(merged),
        "values_sum": sum(values),
        "results_len": len(results),
    }
