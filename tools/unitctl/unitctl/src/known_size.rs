use bytes::Bytes;
use futures::Stream;
use http_body_util::Full;
use std::io;
use std::io::{Cursor, Read};
use std::pin::Pin;
use std::task::{Context, Poll};

pub enum KnownSize {
    Vec(Vec<u8>),
    Read(Box<dyn Read + Send>, u64),
    String(String),
    Empty,
}

impl KnownSize {
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Returns the declared size. For `KnownSize::Read` this is the caller-supplied estimate and
    /// may differ from the number of bytes actually produced — use [`into_full_body`] to get the
    /// true byte count after materialization.
    pub fn len(&self) -> u64 {
        match self {
            KnownSize::Vec(v) => v.len() as u64,
            KnownSize::Read(_, size) => *size,
            KnownSize::String(s) => s.len() as u64,
            KnownSize::Empty => 0,
        }
    }

    /// Materialize the body into a `Full<Bytes>` and return the **actual** byte count.
    ///
    /// Unlike the infallible `From<KnownSize> for Full<Bytes>`, this method surfaces I/O errors
    /// that may occur when reading from `KnownSize::Read`, and returns a `Content-Length` value
    /// derived from the bytes that were truly produced rather than the declared estimate.
    pub fn into_full_body(self) -> io::Result<(Full<Bytes>, u64)> {
        match self {
            KnownSize::Empty => Ok((Full::new(Bytes::new()), 0)),
            KnownSize::Vec(v) => {
                let len = v.len() as u64;
                Ok((Full::new(Bytes::from(v)), len))
            }
            KnownSize::String(s) => {
                let len = s.len() as u64;
                Ok((Full::new(Bytes::from(s)), len))
            }
            KnownSize::Read(mut r, _declared_len) => {
                let mut buf = Vec::new();
                r.read_to_end(&mut buf)?;
                let len = buf.len() as u64;
                Ok((Full::new(Bytes::from(buf)), len))
            }
        }
    }
}

impl Stream for KnownSize {
    type Item = io::Result<Vec<u8>>;

    fn poll_next(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let buf = &mut [0u8; 1024];

        if let KnownSize::Read(r, _) = self.get_mut() {
            return match r.read(buf) {
                Ok(0) => Poll::Ready(None),
                Ok(n) => Poll::Ready(Some(Ok(buf[..n].to_vec()))),
                Err(e) => Poll::Ready(Some(Err(e))),
            };
        }

        panic!("not implemented")
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (0, Some(self.len() as usize))
    }
}

impl From<KnownSize> for Box<dyn Read + Send> {
    fn from(value: KnownSize) -> Self {
        match value {
            KnownSize::Vec(v) => Box::new(Cursor::new(v)),
            KnownSize::Read(r, _) => r,
            KnownSize::String(s) => Box::new(Cursor::new(s)),
            KnownSize::Empty => Box::new(Cursor::new(Vec::new())),
        }
    }
}

impl From<KnownSize> for Full<Bytes> {
    /// Infallible conversion. For `KnownSize::Read` this eagerly buffers the reader and
    /// **panics** on I/O error (use [`KnownSize::into_full_body`] for a fallible alternative that
    /// also returns the true Content-Length).
    fn from(value: KnownSize) -> Self {
        match value {
            KnownSize::Empty => Full::new(Bytes::new()),
            KnownSize::Vec(v) => Full::new(Bytes::from(v)),
            KnownSize::String(s) => Full::new(Bytes::from(s)),
            KnownSize::Read(mut r, _) => {
                let mut buf = Vec::new();
                r.read_to_end(&mut buf)
                    .expect("KnownSize::Read: I/O error while materializing body");
                Full::new(Bytes::from(buf))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io;

    struct ErrorReader;

    impl io::Read for ErrorReader {
        fn read(&mut self, _buf: &mut [u8]) -> io::Result<usize> {
            Err(io::Error::new(io::ErrorKind::Other, "simulated read error"))
        }
    }

    #[test]
    fn into_full_body_read_produces_correct_bytes() {
        let data = b"hello, world!";
        let reader = io::Cursor::new(data.to_vec());
        let known = KnownSize::Read(Box::new(reader), data.len() as u64);
        let (_, len) = known.into_full_body().expect("should succeed");
        assert_eq!(len, data.len() as u64);
    }

    #[test]
    fn into_full_body_surfaces_read_error() {
        let known = KnownSize::Read(Box::new(ErrorReader), 42);
        let result = known.into_full_body();
        assert!(result.is_err(), "expected an error from the failing reader");
        assert_eq!(result.unwrap_err().kind(), io::ErrorKind::Other);
    }

    #[test]
    fn into_full_body_actual_length_for_short_read() {
        // Declared size 100 but reader only has 5 bytes — returned length must reflect reality.
        let data = vec![0u8; 5];
        let reader = io::Cursor::new(data);
        let known = KnownSize::Read(Box::new(reader), 100);
        let (_, len) = known.into_full_body().expect("should succeed");
        assert_eq!(
            len, 5,
            "content-length should match actual bytes, not declared estimate"
        );
    }

    #[test]
    fn into_full_body_string_length_matches() {
        let s = "hello";
        let known = KnownSize::String(s.to_string());
        let (_, len) = known.into_full_body().expect("should succeed");
        assert_eq!(len, s.len() as u64);
    }

    #[test]
    fn into_full_body_vec_length_matches() {
        let data = vec![1u8, 2, 3, 4, 5];
        let known = KnownSize::Vec(data);
        let (_, len) = known.into_full_body().expect("should succeed");
        assert_eq!(len, 5);
    }

    #[test]
    fn into_full_body_empty_is_zero() {
        let (_, len) = KnownSize::Empty.into_full_body().expect("should succeed");
        assert_eq!(len, 0);
    }
}
