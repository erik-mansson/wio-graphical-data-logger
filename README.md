# wio-graphical-data-logger
Arduino data logger with line chart diagram and file handling in a graphical user interface.

It runs on a Wio Terminal with an analogue Seed Studio Grove sensor connected in the right socket and optionally a speaker in the left socket. (Adjusting pin numbers might perhaps make it run on other hardware with a display.)

### Overview of the GUI
On the live screen, the up & down arrows and middle button control a threshold value. The ratio between the measured value and the threshold is shown by a vertical bar on at the right edge of the screen, and also affects the background colour of the entire screen.

In case a speaker is connected and sound is enabled, the speaker starts beeping at a rate relating to how much the threshold is exceeded.

On the live screen, the left & right arrows control the sampling rate, i.e. the number of samples that are averaged to produce one data point. Actually, a fixed pattern of several raw analogue readouts are made for every sample, and median-values are used in order to suppress some of the high-frequency noise that cheap switching power supplies create. To minize the noise when measuring low voltages or skin resistance (electrodermal activity), you may want to use a battery (USB power bank) instead of a AC-to-DC-converting power supply!

![screenshot_1](https://github.com/erik-mansson/wio-graphical-data-logger/assets/16100116/f326f2c1-a22d-4363-ab6f-20702af3b048) ![screenshot_2](https://github.com/erik-mansson/wio-graphical-data-logger/assets/16100116/0e03dfed-f8ef-42e4-a0b5-610a1d967a48)

Above the display, there are three buttons whose function varies depending on which screen (mode) is shown. In live diagram mode the leftmost buttons toggles the speaker on/off and the middle button starts/stops data recording to a file. The rightmost button is generally the menu button for reaching or leaving the menu screen. A few buttons are labelled already on the menu screen and you can press the right arrow to access full-screen explanations of what all the buttons do in each mode.

On the Menu screen, the up & down arrows allow changing the measurement scale, i.e. how the raw voltage measurements should be transformed for display (and logging). See the `valueFromRaw(double)` function and the `sample_transform` variable for details or to add your own transforms. At start-up, the default sample_transform converts readings from the Grove GSR (galvanic skin response) sensor (Seeed Studio product number 101020052) to conductance, i.e. the inverse of electrical resistance. The Grove moisture sensor Nr. 101020008 seems to behave similarly. For general use, there is a sample_transform option which simply expresses the analogue input as a voltage (in mV).


### File management
If an SD-card is available, data can be logged to a text file with a tab-separated values. Each row contains three columns: a timestamp in seconds, the measured value, and the threshold setting. Such a file should be easy to import into a spreadsheet program like LibreOffice Calc or Microsoft Excel.

The program contains a file browser for navigating the logged files and visualizing their content, including the ability to zoom and scroll within the file's diagram. Since the line chart producer seems to freeze if there are too many points, the file loader automatically decimates the data by using only every _N_<sup>th</sup> point from a large file (adjusted as you zoom in). A file can also be exported to the virtual serial port (a computer connected via USB), in case you don't want to move the SD-card to a computer with card reader. Although some (old) documentation says the Wio only supprts SD cards up to 16 GB, which are hard to buy today, I successfully used a 32 GB card.

![screenshot_3](https://github.com/erik-mansson/wio-graphical-data-logger/assets/16100116/9a4b108d-2c89-420b-882e-3c95c7350177)

Since the Wio has no keyboard, the files are named with automatically incrementing five-digit names, organized in four-letter directories. Rebooting the Wio Terminal means that the next file will get a new directory, and that the timesetamp clock restarts at 0. Although the program allows you to delete files, it does currently not delete directories. To clean up completely you need to plug the SD card into a computer with a card reader and delete the directories from there.

### Installation notes
To transfer the program to your Wio Terminal device, you need to install the Arduino IDE on a regular computer and connect the Wio to the computer with a USB cable. Further instructions on this initial configuration can be found in [Seeed Studio's tutorial](https://wiki.seeedstudio.com/Wio-Terminal-Getting-Started/#getting-started).

For the Arduino IDE to open the program, the repository folder needs to be renamed to match the file name, i.e. "wio_graphical_data_logger" (underscores were just not allowed in the Github repository name).

This project depends on the following non-standard-Arduino libraries:
* "Seeed Arduino FS" https://wiki.seeedstudio.com/Wio-Terminal-FS-Overview/
* "Seeed_Arduino_Linechart" https://wiki.seeedstudio.com/Wio-Terminal-LCD-Linecharts/
* The version of the TFT_eSPI library provided out-of-the-box by the "Seeduino Wio Terminal" board in Arduino IDE! Thus you should _NOT_ use the library manager to install TFT_eSPI (https://wiki.seeedstudio.com/Wio-Terminal-LCD-Graphics/). Otherwise nothing but the screen backlight works!
