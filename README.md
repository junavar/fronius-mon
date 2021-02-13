# fronius-mon
Fronius Inverter monitor, datalogger and power control The purpose of this work is to develop and test and tune a datalogger and control software with the following objectives:

Use a Single Board Computer (or SBC) with minimum power consumption and low cost. I have chosen a Rapsberry Pi Model B

Develop a Linux process that collects data from the inverter and registering them in a Operating System shared memory (SHM) area and in a persistent file.

Develop another Linux process that collects data from the SHM area for purposes of visualization and analysis of power and energy generated.

Timely perform a limitation of the power delivered by the inverter so you can control the energy exported to the grid depending on various factors, mainly controlling the amount of energy exported to grid.

This proyect is part of a bigger proyect to visualize and control the electrical energy of private homes and small bussiness.

Use: 
<p>fronius-mon [-i num_inv] [[-l] [-p pot_inv]] [-d] [dev_file]
<dl>
<dt>-i</dt> <dd>number of inverter in rs422 network/connetion. 1 is the default</dd>
<dt>-l</dt> <dd>limit generating power to avoid export of energy to grid. Requires -p option</dd>
<dt> -p</dt> <dd>nominal power of inverter in watts</dd>
<dt>-d</dt> <dd>display frames for debug</dd>
<dt>dev_file</dt>  <dd>device for rs422. Default is /dev/ttyUSB0")</dd>
</dl>
