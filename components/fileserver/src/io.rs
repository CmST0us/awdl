use std::{
    future::Future,
    io,
    pin::Pin,
    task::{Context, Poll},
    time::Duration,
};
use tokio::{
    io::{AsyncRead, AsyncWrite, ReadBuf},
    net::TcpStream,
    time::{Instant, Sleep},
};

pub struct IdleIo {
    socket: TcpStream,
    timeout: Duration,
    deadline: Pin<Box<Sleep>>,
}
impl IdleIo {
    pub fn new(socket: TcpStream, timeout: Duration) -> Self {
        Self {
            socket,
            timeout,
            deadline: Box::pin(tokio::time::sleep(timeout)),
        }
    }
    fn progress(&mut self) {
        self.deadline.as_mut().reset(Instant::now() + self.timeout);
    }
    fn pending<T>(&mut self, cx: &mut Context<'_>) -> Poll<io::Result<T>> {
        match self.deadline.as_mut().poll(cx) {
            Poll::Ready(()) => Poll::Ready(Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "连接读写空闲超时",
            ))),
            Poll::Pending => Poll::Pending,
        }
    }
}
impl AsyncRead for IdleIo {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let before = buf.filled().len();
        match Pin::new(&mut self.socket).poll_read(cx, buf) {
            Poll::Pending => self.pending(cx),
            result => {
                if buf.filled().len() > before {
                    self.progress();
                }
                result
            }
        }
    }
}
impl AsyncWrite for IdleIo {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bytes: &[u8],
    ) -> Poll<io::Result<usize>> {
        match Pin::new(&mut self.socket).poll_write(cx, bytes) {
            Poll::Pending => self.pending(cx),
            result => {
                if matches!(result, Poll::Ready(Ok(n)) if n > 0) {
                    self.progress();
                }
                result
            }
        }
    }
    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.socket).poll_flush(cx)
    }
    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.socket).poll_shutdown(cx)
    }
}
