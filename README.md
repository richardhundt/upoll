# NAME

upoll - epoll for the epoll impaired

# DESCRIPTION

Wraps `epoll`, `poll` and `select` providing a consistent interface
modelled after `epoll`. The reason for this madness is that although
there are plenty of event loop implementations out there (libuv,
libev, libevent, etc.), I couldn't find one that just lets me poll
via a pull-style API.

See the example for usage details. Also, it's really very young. Expect segfaults
and other weirdness.

# LICENSE

I don't care. Just don't sue me if it eats your laundry. Okay, here it is:

```
The MIT License (MIT)

Copyright (c) 2014 Richard Hundt

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

