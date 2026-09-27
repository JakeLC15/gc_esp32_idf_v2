esp32 wrover with external sd module.
Reads mp3 files from sd, buffers data, decodes to pcm and buffers.
Connects to BT Classic speaker and streams pcm. Option to save speaker name as priority connection, otherwise connects to closest.
Sends id3 data on request over i2c to esphome based display.
Sends status on request over i2c and change sequential / shuffle play.

SD has no directory, just songs, skips non mp3.
