# fronius-mon
Fronius Inverter monitor, datalogger and power control The purpose of this work is to develop and test and tune a datalogger and control software with the following objectives:

Use a Single Board Computer (or SBC) with minimum power consumption and low cost. I have chosen a Rapsberry Pi Model B

Develop a Linux process that collects data from the inverter and registering them in a Operating System shared memory (SHM) area and in a persistent file.

Develop another Linux process that collects data from the SHM area for purposes of visualization and analysis of power and energy generated.

Timely perform a limitation of the power delivered by the inverter so you can control the energy exported to the grid depending on various factors, mainly controlling the amount of energy exported to grid.

This proyect is part of a bigger proyect to visualize and control the electrical energy of private homes and small bussiness.

Use: fronius-mon [-i num_inv] [[-l] [-p pot_inv]] [-d] [dev_file]
<p> -i        number of inverter in rs422 network/connetion. 1 is the default
<p> -l        limit generating power to avoid export of energy to grid. Requires -p option
<p> -p        nominal power of inverter in watts
<p> -d        display frames for debug
<p> dev_file  device for rs422. Default is /dev/ttyUSB0")

