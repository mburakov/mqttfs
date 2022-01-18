# mqttfs

This is a small FUSE filesystem that exposes MQTT topics from a (remote) broker
as a file tree. The content of exposed files is updated in realtime as soon as
broker sends an update via MQTT subscription.

## Building on Linux

mqttfs depends on fuse3 and libmosquitto. Once you have these installed, just
```
make
```

## Building anywhere else

I don't care about any other platforms except Linux, so you are on your own.

## Running

Run it as any other FUSE filesystem binary providing a mountpoint. Optionally
provide host and port of your MQTT broker on the commandline. Without any
arguments it will try to connect to localhost on the standard port 1883:
```
./mqttfs --host=localhost --port=1883 /mount/point
```

## Usage

Anything that you can imagine. I.e. try this, in the first terminal:
```
mkdir /tmp/mqttfs
./mqttfs --host=pi /tmp/mqttfs
```

In the second terminal:
```
cd /tmp/mqttfs
python -m http.server 8000
```

In the third terminal:
```
curl http://localhost:8000/zigbee2mqtt/bridge/state
```

## Bugs

Yes.
