<p>
  Freeswitch ASR module for the Google cloud Speech-to-Text service, based on v1-rest api with VAD. <br>
  P.S.: Might troubles (crashes on http requests) under Ubuntu 22.04.2 LTS, seems it depends on some system libs...(the reasons are still being investigated...)
</p>

### Usage example
```
<extension name="asr-test">
  <condition field="destination_number" expression="^(3222)$">
    <action application="answer"/>
    <action application="sleep" data="1000"/>
    <action application="play_and_detect_speech" data="/tmp/test2.wav detect:google {lang=en}"/>.
    <action application="sleep" data="1000"/>
    <action application="log" data="CRIT SPEECH_RESULT=${detect_speech_result}"/>
    <action application="sleep" data="1000"/>
    <action application="hangup"/>
 </condition>
</extension>

```
