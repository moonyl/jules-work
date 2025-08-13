console.log("Client script loaded.");

const video = document.getElementById('video');

if (!window.MediaSource) {
    alert('Media Source Extensions API is not supported in this browser.');
}

const mediaSource = new MediaSource();
video.src = URL.createObjectURL(mediaSource);

let sourceBuffer = null;
const bufferQueue = [];
let isInitSegmentReceived = false;

mediaSource.addEventListener('sourceopen', () => {
    console.log('MediaSource opened.');
    // The codec string depends on the video stream.
    // 'avc1.42E01E' is a common value for H.264 baseline profile.
    // This might need to be adjusted based on the specific RTSP stream.
    const codec = 'video/mp4; codecs="avc1.42E01E"';

    if (MediaSource.isTypeSupported(codec)) {
        sourceBuffer = mediaSource.addSourceBuffer(codec);
        sourceBuffer.addEventListener('updateend', () => {
            // When an update is complete, process the next item in the queue.
            if (bufferQueue.length > 0 && !sourceBuffer.updating) {
                sourceBuffer.appendBuffer(bufferQueue.shift());
            }
        });
    } else {
        console.error(`Codec not supported: ${codec}`);
    }

    // Connect to WebSocket after MediaSource is ready
    setupWebSocket();
});

function setupWebSocket() {
    const ws = new WebSocket('ws://' + window.location.host + '/stream');
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        console.log('WebSocket connection opened.');
        // Request the initialization segment from the server
        console.log('Requesting init segment...');
        ws.send('get_init');
    };

    ws.onmessage = (event) => {
        const data = event.data;
        if (sourceBuffer.updating || bufferQueue.length > 0) {
            bufferQueue.push(data);
        } else {
            sourceBuffer.appendBuffer(data);
        }
    };

    ws.onclose = () => {
        console.log('WebSocket connection closed.');
        if (mediaSource.readyState === 'open') {
            mediaSource.endOfStream();
        }
    };

    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
    };
}
