import os
from dataclasses import dataclass


@dataclass
class Point:
    x: int = 0
    y: int = 0

    def norm(self) -> float:
        """Euclidean length."""
        return (self.x ** 2 + self.y ** 2) ** 0.5


def main():
    # print a greeting
    name = os.environ.get("USER", 'world')
    for i in range(3):
        if i % 2 == 0:
            print(f"hello {name} {i}")
    return 0
