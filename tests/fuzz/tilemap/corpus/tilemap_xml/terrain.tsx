<?xml version="1.0" encoding="UTF-8"?>
<tileset version="1.10" tiledversion="1.10.2" name="terrain" tilewidth="16" tileheight="16" tilecount="8" columns="4">
 <image source="terrain.png" width="64" height="32"/>
 <tile id="1">
  <animation>
   <frame tileid="1" duration="200"/>
   <frame tileid="2" duration="300"/>
  </animation>
 </tile>
 <tile id="3">
  <objectgroup draworder="index" id="2">
   <object id="1" x="0" y="0">
    <polygon points="0,16 16,0 16,16"/>
   </object>
  </objectgroup>
 </tile>
 <tile id="6">
  <objectgroup draworder="index" id="2">
   <object id="1" x="0" y="0" width="16" height="16"/>
  </objectgroup>
 </tile>
 <tile id="7">
  <objectgroup draworder="index" id="2">
   <object id="1" x="0" y="0">
    <polyline points="0,3 1.25,4 2.5,3 3.75,4 5,3 6.25,4 7.5,3 8.75,4 10,3 11.25,4 12.5,3 13.75,4"/>
   </object>
  </objectgroup>
 </tile>
</tileset>
