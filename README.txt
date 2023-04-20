Compilation Information
- Please run "sudo make TARGET=cc26x0-cc13x0 BOARD=sensortag/cc2650 PORT=/dev/ttyACM0 program_name" to compile the appropriate "program_name" for each part.
- For Task 1, run "sudo make TARGET=cc26x0-cc13x0 BOARD=sensortag/cc2650 PORT=/dev/ttyACM0 nbr"
- For Task 2, run "sudo make TARGET=cc26x0-cc13x0 BOARD=sensortag/cc2650 PORT=/dev/ttyACM0 nbr-part2-sender" and "sudo make TARGET=cc26x0-cc13x0 BOARD=sensortag/cc2650 PORT=/dev/ttyACM0 nbr-part2-requester" 

Following the above steps will ensure compilation is done for all files.

To test for Task 2, please choose a device as the light-sensing node and identify its link address. 
Then, modify the "light_addr" variable in both source files to match the address and compile the files.

Flash the appropriate files onto each device. The light-sensing node should have the compiled "nbr-part2-sender" file flashed, and the other nodes (data mules) will have the compiled "nbr-part2-requester" file flashed.