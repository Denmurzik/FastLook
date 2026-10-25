# FastLook Markdown

A paragraph with **bold**, *italic*, ~~strike~~, `inline code` and a [link to GitHub](https://github.com/Denmurzik/FastLook). A bad link [file](file:///C:/Windows/notepad.exe) is ignored.

## Lists

- First item
- Second item with a nested list:
  - Nested one
  - Nested two
- Third item

1. One
2. Two
3. Three

- [x] Done task
- [ ] Open task

> A quote that explains something
> over two lines.

### Code

```cpp
#include <iostream>

// Prints a greeting
int main()
{
    std::cout << "Hello, " << 42 << '\n';
    return 0;
}
```

---

#### Images

Local image: ![local gradient](local.png)

Remote image: ![remote logo alt text](https://example.com/logo.png)

Inline HTML: <b>raw</b> stays text.

<div>An HTML block</div>
