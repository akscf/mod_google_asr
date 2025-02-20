<p>
  Google Speech-To-Text service for the Freeswitch. <br>
</p>

### Usage example
```
<extension name="asr-test">
  <condition field="destination_number" expression="^(3222)$">
    <action application="answer"/>
    <action application="sleep" data="1000"/>
    <action application="play_and_detect_speech" data="/tmp/test2.wav detect:google {lang=en}"/>
    <action application="sleep" data="1000"/>
    <action application="log" data="CRIT SPEECH_RESULT=${detect_speech_result}"/>
    <action application="sleep" data="1000"/>
    <action application="hangup"/>
 </condition>
</extension>

```

### mod_quickjs example
```javascript
session.ttsEngine= 'google';
session.asrEngine= 'google';

var txt = session.sayAndDetectSpeech('Hello, how can I help you?', 10);
consoleLog('info', "TEXT: " + txt);
```

