import { $ } from '../$';
import { def } from '../utils';
import { EncodedVideoChunk } from './encoded-video-chunk';
import { VideoDecoder } from './video-decoder';
import { VideoFrame } from './video-frame';

export { EncodedVideoChunk } from './encoded-video-chunk';
export type {
	EncodedVideoChunkInit,
	EncodedVideoChunkType,
} from './encoded-video-chunk';
export { VideoDecoder } from './video-decoder';
export type {
	VideoDecoderConfig,
	VideoDecoderInit,
	VideoDecoderState,
	VideoDecoderSupport,
} from './video-decoder';
export { VideoFrame } from './video-frame';

// The native half owns the read-only attributes (state, codec,
// numberOfFrames, numberOfOutputFrames, decodeQueueSize on VideoDecoder;
// codedWidth, codedHeight, displayWidth, displayHeight, timestamp, duration,
// format on VideoFrame) and installs them on the prototypes.
$.webcodecsVideoDecoderInitClass(VideoDecoder);
$.webcodecsVideoFrameInitClass(VideoFrame);

def(VideoDecoder);
def(VideoFrame);
def(EncodedVideoChunk);
