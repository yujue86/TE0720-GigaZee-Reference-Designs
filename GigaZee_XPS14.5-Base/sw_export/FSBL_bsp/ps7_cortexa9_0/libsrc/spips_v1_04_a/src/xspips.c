/******************************************************************************
*
* (c) Copyright 2010-2013 Xilinx, Inc. All rights reserved.
*
* This file contains confidential and proprietary information of Xilinx, Inc.
* and is protected under U.S. and international copyright and other
* intellectual property laws.
*
* DISCLAIMER
* This disclaimer is not a license and does not grant any rights to the
* materials distributed herewith. Except as otherwise provided in a valid
* license issued to you by Xilinx, and to the maximum extent permitted by
* applicable law: (1) THESE MATERIALS ARE MADE AVAILABLE "AS IS" AND WITH ALL
* FAULTS, AND XILINX HEREBY DISCLAIMS ALL WARRANTIES AND CONDITIONS, EXPRESS,
* IMPLIED, OR STATUTORY, INCLUDING BUT NOT LIMITED TO WARRANTIES OF
* MERCHANTABILITY, NON-INFRINGEMENT, OR FITNESS FOR ANY PARTICULAR PURPOSE;
* and (2) Xilinx shall not be liable (whether in contract or tort, including
* negligence, or under any other theory of liability) for any loss or damage
* of any kind or nature related to, arising under or in connection with these
* materials, including for any direct, or any indirect, special, incidental,
* or consequential loss or damage (including loss of data, profits, goodwill,
* or any type of loss or damage suffered as a result of any action brought by
* a third party) even if such damage or loss was reasonably foreseeable or
* Xilinx had been advised of the possibility of the same.
*
* CRITICAL APPLICATIONS
* Xilinx products are not designed or intended to be fail-safe, or for use in
* any application requiring fail-safe performance, such as life-support or
* safety devices or systems, Class III medical devices, nuclear facilities,
* applications related to the deployment of airbags, or any other applications
* that could lead to death, personal injury, or severe property or
* environmental damage (individually and collectively, "Critical
* Applications"). Customer assumes the sole risk and liability of any use of
* Xilinx products in Critical Applications, subject only to applicable laws
* and regulations governing limitations on product liability.
*
* THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS PART OF THIS FILE
* AT ALL TIMES.
*
******************************************************************************/
/*****************************************************************************/
/**
*
* @file xspips.c
*
* Contains implements the interface functions of the XSpiPs driver.
* See xspips.h for a detailed description of the device and driver.
*
* <pre>
* MODIFICATION HISTORY:
*
* Ver   Who    Date     Changes
* ----- ------ -------- -----------------------------------------------
* 1.00  drg/jz 01/25/10 First release
* 1.01	sg     03/07/12 Updated the code to always clear the relevant bits
*			before writing to config register.
*			Always clear the slave select bits before write and
*			clear the bits to no slave at the end of transfer
*			Modified the Polled transfer transmit/receive logic.
*			Tx should wait on TXOW Interrupt and Rx on RXNEMTY.
* 1.03	sg     09/21/12 Added memory barrier dmb in polled transfer and
*			interrupt handler to overcome the clock domain
*			crossing issue in the controller. For CR #679252.
* 2.00a	sg     01/30/13 Changed SPI transfer logic for polled and interrupt
*			modes to be based on filled tx fifo count and receive
*			based on it. RXNEMPTY interrupt is not used.
*			SetSlaveSelect API logic is modified to drive the bit
*			position low based on the slave select value
*			requested. GetSlaveSelect API will return the value
*			based on bit position that is low.
*
* </pre>
*
******************************************************************************/

/***************************** Include Files *********************************/

#include "xspips.h"

/************************** Constant Definitions *****************************/


/**************************** Type Definitions *******************************/


/***************** Macros (Inline Functions) Definitions *********************/

/****************************************************************************/
/*
*
* Send one byte to the currently selected slave. A byte of data is written to
* transmit FIFO/register.
*
* @param	BaseAddress is the  base address of the device
*
* @return	None.
*
* @note		C-Style signature:
*		void XSpiPs_SendByte(u32 BaseAddress, u8 Data);
*
*****************************************************************************/
#define XSpiPs_SendByte(BaseAddress, Data) \
		XSpiPs_Out32((BaseAddress) + XSPIPS_TXD_OFFSET, (Data))

/****************************************************************************/
/*
*
* Receive one byte from the device's receive FIFO/register. It is assumed
* that the byte is already available.
*
* @param	BaseAddress is the  base address of the device
*
* @return	The byte retrieved from the receive FIFO/register.
*
* @note		C-Style signature:
*		u8 XSpiPs_RecvByte(u32 BaseAddress);
*
*****************************************************************************/
#define XSpiPs_RecvByte(BaseAddress) \
		(u8)XSpiPs_In32((BaseAddress) + XSPIPS_RXD_OFFSET)

/************************** Function Prototypes ******************************/

static void StubStatusHandler(void *CallBackRef, u32 StatusEvent,
				unsigned ByteCount);

/************************** Variable Definitions *****************************/


/*****************************************************************************/
/**
*
* Initializes a specific XSpiPs instance such that the driver is ready to use.
*
* The state of the device after initialization is:
*   - Device is disabled
*   - Slave mode
*   - Active high clock polarity
*   - Clock phase 0
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
* @param	ConfigPtr is a reference to a structure containing information
*		about a specific SPI device. This function initializes an
*		InstancePtr object for a specific device specified by the
*		contents of Config. This function can initialize multiple
*		instance objects with the use of multiple calls giving different
*		Config information on each call.
* @param	EffectiveAddr is the device base address in the virtual memory
*		address space. The caller is responsible for keeping the address
*		mapping from EffectiveAddr to the device physical base address
*		unchanged once this function is invoked. Unexpected errors may
*		occur if the address mapping changes after this function is
*		called. If address translation is not used, use
*		ConfigPtr->Config.BaseAddress for this device.
*
* @return
*		- XST_SUCCESS if successful.
*		- XST_DEVICE_IS_STARTED if the device is already started.
*		It must be stopped to re-initialize.
*
* @note		None.
*
******************************************************************************/
int XSpiPs_CfgInitialize(XSpiPs *InstancePtr, XSpiPs_Config *ConfigPtr,
				u32 EffectiveAddr)
{
	Xil_AssertNonvoid(InstancePtr != NULL);
	Xil_AssertNonvoid(ConfigPtr != NULL);

	/*
	 * If the device is busy, disallow the initialize and return a status
	 * indicating it is already started. This allows the user to stop the
	 * device and re-initialize, but prevents a user from inadvertently
	 * initializing. This assumes the busy flag is cleared at startup.
	 */
	if (InstancePtr->IsBusy == TRUE) {
		return XST_DEVICE_IS_STARTED;
	}

	/*
	 * Set some default values.
	 */
	InstancePtr->IsBusy = FALSE;

	InstancePtr->Config.BaseAddress = EffectiveAddr;
	InstancePtr->StatusHandler = StubStatusHandler;

	InstancePtr->SendBufferPtr = NULL;
	InstancePtr->RecvBufferPtr = NULL;
	InstancePtr->RequestedBytes = 0;
	InstancePtr->RemainingBytes = 0;
	InstancePtr->IsReady = XIL_COMPONENT_IS_READY;

	/*
	 * Reset the SPI device to get it into its initial state. It is
	 * expected that device configuration will take place after this
	 * initialization is done, but before the device is started.
	 */
	XSpiPs_Reset(InstancePtr);

	return XST_SUCCESS;
}


/*****************************************************************************/
/**
*
* Resets the SPI device. Reset must only be called after the driver has been
* initialized. The configuration of the device after reset is the same as its
* configuration after initialization.  Any data transfer that is in progress
* is aborted.
*
* The upper layer software is responsible for re-configuring (if necessary)
* and restarting the SPI device after the reset.
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
void XSpiPs_Reset(XSpiPs *InstancePtr)
{
	Xil_AssertVoid(InstancePtr != NULL);
	Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

	/*
	 * Abort any transfer that is in progress
	 */
	XSpiPs_Abort(InstancePtr);

        /*
         * Reset any values that are not reset by the hardware reset such that
         * the software state matches the hardware device
         */
        XSpiPs_WriteReg(InstancePtr->Config.BaseAddress, XSPIPS_CR_OFFSET,
                        XSPIPS_CR_RESET_STATE);

}

/*****************************************************************************/
/**
*
* Transfers specified data on the SPI bus. If the SPI device is configured as
* a master, this function initiates bus communication and sends/receives the
* data to/from the selected SPI slave. If the SPI device is configured as a
* slave, this function prepares the buffers to be sent/received when selected
* by a master. For every byte sent, a byte is received. This function should
* be used to perform interrupt based transfers.
*
* The caller has the option of providing two different buffers for send and
* receive, or one buffer for both send and receive, or no buffer for receive.
* The receive buffer must be at least as big as the send buffer to prevent
* unwanted memory writes. This implies that the byte count passed in as an
* argument must be the smaller of the two buffers if they differ in size.
* Here are some sample usages:
* <pre>
*   XSpiPs_Transfer(InstancePtr, SendBuf, RecvBuf, ByteCount)
*	The caller wishes to send and receive, and provides two different
*	buffers for send and receive.
*
*   XSpiPs_Transfer(InstancePtr, SendBuf, NULL, ByteCount)
*	The caller wishes only to send and does not care about the received
*	data. The driver ignores the received data in this case.
*
*   XSpiPs_Transfer(InstancePtr, SendBuf, SendBuf, ByteCount)
*	The caller wishes to send and receive, but provides the same buffer
*	for doing both. The driver sends the data and overwrites the send
*	buffer with received data as it transfers the data.
*
*   XSpiPs_Transfer(InstancePtr, RecvBuf, RecvBuf, ByteCount)
*	The caller wishes to only receive and does not care about sending
*	data.  In this case, the caller must still provide a send buffer, but
*	it can be the same as the receive buffer if the caller does not care
*	what it sends.  The device must send N bytes of data if it wishes to
*	receive N bytes of data.
* </pre>
* Although this function takes entire buffers as arguments, the driver can only
* transfer a limited number of bytes at a time, limited by the size of the
* FIFO. A call to this function only starts the transfer, then subsequent
* transfers of the data is performed by the interrupt service routine until
* the entire buffer has been transferred. The status callback function is
* called when the entire buffer has been sent/received.
*
* This function is non-blocking. As a master, the SetSlaveSelect function must
* be called prior to this function.
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
* @param	SendBufPtr is a pointer to a buffer of data for sending.
*		This buffer must not be NULL.
* @param	RecvBufPtr is a pointer to a buffer for received data.
*		This argument can be NULL if do not care about receiving.
* @param	ByteCount contains the number of bytes to send/receive.
*		The number of bytes received always equals the number of bytes
*		sent.
*
* @return
*		- XST_SUCCESS if the buffers are successfully handed off to the
*		device for transfer.
*		- XST_DEVICE_BUSY indicates that a data transfer is already in
*		progress. This is determined by the driver.
*
* @note
*
* This function is not thread-safe.  The higher layer software must ensure that
* no two threads are transferring data on the SPI bus at the same time.
*
******************************************************************************/
int XSpiPs_Transfer(XSpiPs *InstancePtr, u8 *SendBufPtr,
			u8 *RecvBufPtr, unsigned ByteCount)
{
	u32 ConfigReg;
	u8 TransCount = 0;

	/*
	 * The RecvBufPtr argument can be null
	 */
	Xil_AssertNonvoid(InstancePtr != NULL);
	Xil_AssertNonvoid(SendBufPtr != NULL);
	Xil_AssertNonvoid(ByteCount > 0);
	Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

	/*
	 * Check whether there is another transfer in progress. Not thread-safe.
	 */
	if (InstancePtr->IsBusy) {
		return XST_DEVICE_BUSY;
	}

	/*
	 * Set the busy flag, which will be cleared in the ISR when the
	 * transfer is entirely done.
	 */
	InstancePtr->IsBusy = TRUE;

	/*
	 * Set up buffer pointers.
	 */
	InstancePtr->SendBufferPtr = SendBufPtr;
	InstancePtr->RecvBufferPtr = RecvBufPtr;

	InstancePtr->RequestedBytes = ByteCount;
	InstancePtr->RemainingBytes = ByteCount;

	/*
	 * If manual chip select mode, initialize the slave select value.
	 */
	if (XSpiPs_IsManualChipSelect(InstancePtr)) {
		ConfigReg = XSpiPs_ReadReg(InstancePtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET);
		/*
		 * Set the slave select value.
		 */
		ConfigReg &= ~XSPIPS_CR_SSCTRL_MASK;
		ConfigReg |= InstancePtr->SlaveSelect;
		XSpiPs_WriteReg(InstancePtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET, ConfigReg);
	}

	/*
	 * Enable the device.
	 */
	XSpiPs_Enable(InstancePtr);

	/*
	 * Clear all the interrrupts.
	 */
	XSpiPs_WriteReg(InstancePtr->Config.BaseAddress, XSPIPS_SR_OFFSET,
			XSPIPS_IXR_WR_TO_CLR_MASK);

	/*
	 * Fill the TXFIFO with as many bytes as it will take (or as many as
	 * we have to send).
	 */
	while ((InstancePtr->RemainingBytes > 0) &&
		(TransCount < XSPIPS_FIFO_DEPTH)) {
		XSpiPs_SendByte(InstancePtr->Config.BaseAddress,
			  *InstancePtr->SendBufferPtr);
		InstancePtr->SendBufferPtr++;
		InstancePtr->RemainingBytes--;
		TransCount++;
	}

	/*
	 * Enable interrupts (connecting to the interrupt controller and
	 * enabling interrupts should have been done by the caller).
	 */
	XSpiPs_WriteReg(InstancePtr->Config.BaseAddress,
			XSPIPS_IER_OFFSET, XSPIPS_IXR_DFLT_MASK);

	/*
	 * If master mode and manual start mode, issue manual start command
	 * to start the transfer.
	 */
	if (XSpiPs_IsManualStart(InstancePtr)
		&& XSpiPs_IsMaster(InstancePtr)) {
		ConfigReg = XSpiPs_ReadReg(InstancePtr->Config.BaseAddress,
					   XSPIPS_CR_OFFSET);
			ConfigReg |= XSPIPS_CR_MANSTRT_MASK;
		XSpiPs_WriteReg(InstancePtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET, ConfigReg);
	}

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
* Transfers specified data on the SPI bus in polled mode.
*
* The caller has the option of providing two different buffers for send and
* receive, or one buffer for both send and receive, or no buffer for receive.
* The receive buffer must be at least as big as the send buffer to prevent
* unwanted memory writes. This implies that the byte count passed in as an
* argument must be the smaller of the two buffers if they differ in size.
* Here are some sample usages:
* <pre>
*   XSpiPs_PolledTransfer(InstancePtr, SendBuf, RecvBuf, ByteCount)
*	The caller wishes to send and receive, and provides two different
*	buffers for send and receive.
*
*   XSpiPs_PolledTransfer(InstancePtr, SendBuf, NULL, ByteCount)
*	The caller wishes only to send and does not care about the received
*	data. The driver ignores the received data in this case.
*
*   XSpiPs_PolledTransfer(InstancePtr, SendBuf, SendBuf, ByteCount)
*	The caller wishes to send and receive, but provides the same buffer
*	for doing both. The driver sends the data and overwrites the send
*	buffer with received data as it transfers the data.
*
*   XSpiPs_PolledTransfer(InstancePtr, RecvBuf, RecvBuf, ByteCount)
*	The caller wishes to only receive and does not care about sending
*	data.  In this case, the caller must still provide a send buffer, but
*	it can be the same as the receive buffer if the caller does not care
*	what it sends.  The device must send N bytes of data if it wishes to
*	receive N bytes of data.
*
* </pre>
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
* @param	SendBufPtr is a pointer to a buffer of data for sending.
*		This buffer must not be NULL.
* @param	RecvBufPtr is a pointer to a buffer for received data.
*		This argument can be NULL if do not care about receiving.
* @param	ByteCount contains the number of bytes to send/receive.
*		The number of bytes received always equals the number of bytes
*		sent.

* @return
*		- XST_SUCCESS if the buffers are successfully handed off to the
*		device for transfer.
*		- XST_DEVICE_BUSY indicates that a data transfer is already in
*		progress. This is determined by the driver.
*
* @note
*
* This function is not thread-safe.  The higher layer software must ensure that
* no two threads are transferring data on the SPI bus at the same time.
*
******************************************************************************/
int XSpiPs_PolledTransfer(XSpiPs *InstancePtr, u8 *SendBufPtr,
				u8 *RecvBufPtr, unsigned ByteCount)
{
	u32 StatusReg;
	u32 ConfigReg;
	u32 TransCount;

	/*
	 * The RecvBufPtr argument can be NULL.
	 */
	Xil_AssertNonvoid(InstancePtr != NULL);
	Xil_AssertNonvoid(SendBufPtr != NULL);
	Xil_AssertNonvoid(ByteCount > 0);
	Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

	/*
	 * Check whether there is another transfer in progress. Not thread-safe.
	 */
	if (InstancePtr->IsBusy) {
		return XST_DEVICE_BUSY;
	}

	/*
	 * Set the busy flag, which will be cleared when the transfer is
	 * entirely done.
	 */
	InstancePtr->IsBusy = TRUE;

	/*
	 * Set up buffer pointers.
	 */
	InstancePtr->SendBufferPtr = SendBufPtr;
	InstancePtr->RecvBufferPtr = RecvBufPtr;

	InstancePtr->RequestedBytes = ByteCount;
	InstancePtr->RemainingBytes = ByteCount;

	/*
	 * If manual chip select mode, initialize the slave select value.
	 */
	if (XSpiPs_IsManualChipSelect(InstancePtr)) {
		ConfigReg = XSpiPs_ReadReg(InstancePtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET);
		/*
		 * Set the slave select value.
		 */
		ConfigReg &= ~XSPIPS_CR_SSCTRL_MASK;
		ConfigReg |= InstancePtr->SlaveSelect;
		XSpiPs_WriteReg(InstancePtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET, ConfigReg);
	}

	/*
	 * Enable the device.
	 */
	XSpiPs_Enable(InstancePtr);

	while((InstancePtr->RemainingBytes > 0) ||
		(InstancePtr->RequestedBytes > 0)) {
		TransCount = 0;
		/*
		 * Fill the TXFIFO with as many bytes as it will take (or as
		 * many as we have to send).
		 */
		while ((InstancePtr->RemainingBytes > 0) &&
			(TransCount < XSPIPS_FIFO_DEPTH)) {
			XSpiPs_SendByte(InstancePtr->Config.BaseAddress,
					*InstancePtr->SendBufferPtr);
			InstancePtr->SendBufferPtr++;
			InstancePtr->RemainingBytes--;
			++TransCount;
		}

		/*
		 * If master mode and manual start mode, issue manual start
		 * command to start the transfer.
		 */
		if (XSpiPs_IsManualStart(InstancePtr)
			&& XSpiPs_IsMaster(InstancePtr)) {
			ConfigReg = XSpiPs_ReadReg(
					InstancePtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET);
			ConfigReg |= XSPIPS_CR_MANSTRT_MASK;
			XSpiPs_WriteReg(InstancePtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET, ConfigReg);
		}

		/*
		 * Wait for the transfer to finish by polling Tx fifo status.
		 */
		do {
			StatusReg = XSpiPs_ReadReg(
					InstancePtr->Config.BaseAddress,
					XSPIPS_SR_OFFSET);
		} while ((StatusReg & XSPIPS_IXR_TXOW_MASK) == 0);

		/*
		 * A transmit has just completed. Process received data and
		 * check for more data to transmit.
		 * First get the data received as a result of the transmit
		 * that just completed. Receive data based on the
		 * count obtained while filling tx fifo. Always get the
		 * received data, but only fill the receive buffer if it
		 * points to something (the upper layer software may not
		 * care to receive data).
		 */
		while (TransCount) {
			u8 TempData;
			TempData = XSpiPs_RecvByte(
				InstancePtr->Config.BaseAddress);
			if (InstancePtr->RecvBufferPtr != NULL) {
				*InstancePtr->RecvBufferPtr++ = (u8) TempData;
			}
			InstancePtr->RequestedBytes--;
			--TransCount;
		}
	}

	/*
	 * Clear the slave selects now, before terminating the transfer.
	 */
	if (XSpiPs_IsManualChipSelect(InstancePtr)) {
		ConfigReg = XSpiPs_ReadReg(InstancePtr->Config.BaseAddress,
					XSPIPS_CR_OFFSET);
		ConfigReg |= XSPIPS_CR_SSCTRL_MASK;
		XSpiPs_WriteReg(InstancePtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET, ConfigReg);
	}

	/*
	 * Clear the busy flag.
	 */
	InstancePtr->IsBusy = FALSE;

	/*
	 * Disable the device.
	 */
	XSpiPs_Disable(InstancePtr);

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* Selects or deselect the slave with which the master communicates. This setting
* affects the SPI_ss_outN signals. The behavior depends on the setting of the
* CR_SSDECEN bit. If CR_SSDECEN is 0, the SPI_ss_outN bits will be output with a
* single signal low. If CR_SSDECEN is 1, the SPI_ss_outN bits will reflect the
* value set.
*
* The user is not allowed to deselect the slave while a transfer is in progress.
* If no transfer is in progress, the user can select a new slave, which
* implicitly deselects the current slave. In order to explicitly deselect the
* current slave, a value of all 1's, 0x0F can be passed in as the argument to
* the function.
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
* @param	SlaveSel is an 3-bit mask with a 1 in the bit position of the
*		slave being selected. Only one slave can be selected.
*
* @return
*		- XST_SUCCESS if the slave is selected or deselected
*		successfully.
*		- XST_DEVICE_BUSY if a transfer is in progress, slave cannot be
*		changed.
*
* @note
*
* This function only sets the slave which will be selected when a transfer
* occurs. The slave is not selected when the SPI is idle. The slave select
* has no affect when the device is configured as a slave.
*
******************************************************************************/
int XSpiPs_SetSlaveSelect(XSpiPs *InstancePtr, u8 SlaveSel)
{
	u32 ConfigReg;

	Xil_AssertNonvoid(InstancePtr != NULL);
	Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);
	Xil_AssertNonvoid(SlaveSel <= XSPIPS_CR_SSCTRL_MAXIMUM);

	/*
	 * Do not allow the slave select to change while a transfer is in
	 * progress. Not thread-safe.
	 */
	if (InstancePtr->IsBusy) {
		return XST_DEVICE_BUSY;
	}

	/*
	 * Set the bit position to low using SlaveSel. Update the Instance
	 * structure member.
	 */
	InstancePtr->SlaveSelect = ((~(1 << SlaveSel)) & \
			XSPIPS_CR_SSCTRL_MAXIMUM) << XSPIPS_CR_SSCTRL_SHIFT;

	/*
	 * Read the config register, update the slave select value and write
	 * back to config register.
	 */
	ConfigReg = XSpiPs_ReadReg(InstancePtr->Config.BaseAddress,
			 XSPIPS_CR_OFFSET);
	ConfigReg |= InstancePtr->SlaveSelect;
	XSpiPs_WriteReg(InstancePtr->Config.BaseAddress, XSPIPS_CR_OFFSET,
			 ConfigReg);

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* Gets the current slave select setting for the SPI device.
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
*
* @return	The value of the SPI_ss_outN bits in the Config register.
*
* @note		None.
*
******************************************************************************/
u8 XSpiPs_GetSlaveSelect(XSpiPs *InstancePtr)
{
	u32 ConfigReg;
	u8 SlaveSel;

	Xil_AssertNonvoid(InstancePtr != NULL);
	Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

	ConfigReg = InstancePtr->SlaveSelect;
	ConfigReg &= XSPIPS_CR_SSCTRL_MASK;
	ConfigReg >>= XSPIPS_CR_SSCTRL_SHIFT;
	ConfigReg &= XSPIPS_CR_SSCTRL_MAXIMUM;

	/*
	 * Following if-else construct helps to get the slave select value.
	 */
	if ((~ConfigReg) > 0x4)
		SlaveSel = 0xF; /* No slave selected */
	else
		SlaveSel = (~ConfigReg)/2;

	return SlaveSel;
}

/*****************************************************************************/
/**
*
* Sets the status callback function, the status handler, which the driver
* calls when it encounters conditions that should be reported to upper
* layer software. The handler executes in an interrupt context, so it must
* minimize the amount of processing performed. One of the following status
* events is passed to the status handler.
*
* <pre>
* XST_SPI_MODE_FAULT		A mode fault error occurred, meaning the device
*				is selected as slave while being a master.
*
* XST_SPI_TRANSFER_DONE		The requested data transfer is done
*
* XST_SPI_TRANSMIT_UNDERRUN	As a slave device, the master clocked data
*				but there were none available in the transmit
*				register/FIFO. This typically means the slave
*				application did not issue a transfer request
*				fast enough, or the processor/driver could not
*				fill the transmit register/FIFO fast enough.
*
* XST_SPI_RECEIVE_OVERRUN	The SPI device lost data. Data was received
*				but the receive data register/FIFO was full.
*
* XST_SPI_SLAVE_MODE_FAULT	A slave SPI device was selected as a slave
*				while it was disabled. This indicates the
*				master is already transferring data (which is
*				being dropped until the slave application
*				issues a transfer).
* </pre>
* @param	InstancePtr is a pointer to the XSpiPs instance.
* @param	CallBackRef is the upper layer callback reference passed back
*		when the callback function is invoked.
* @param	FuncPtr is the pointer to the callback function.
*
* @return	None.
*
* @note
*
* The handler is called within interrupt context, so it should do its work
* quickly and queue potentially time-consuming work to a task-level thread.
*
******************************************************************************/
void XSpiPs_SetStatusHandler(XSpiPs *InstancePtr, void *CallBackRef,
				XSpiPs_StatusHandler FuncPtr)
{
	Xil_AssertVoid(InstancePtr != NULL);
	Xil_AssertVoid(FuncPtr != NULL);
	Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

	InstancePtr->StatusHandler = FuncPtr;
	InstancePtr->StatusRef = CallBackRef;
}

/*****************************************************************************/
/**
*
* This is a stub for the status callback. The stub is here in case the upper
* layers forget to set the handler.
*
* @param	CallBackRef is a pointer to the upper layer callback reference
* @param	StatusEvent is the event that just occurred.
* @param	ByteCount is the number of bytes transferred up until the event
*		occurred.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void StubStatusHandler(void *CallBackRef, u32 StatusEvent,
				unsigned ByteCount)
{
	(void) CallBackRef;
	(void) StatusEvent;
	(void) ByteCount;

	Xil_AssertVoidAlways();
}

/*****************************************************************************/
/**
*
* The interrupt handler for SPI interrupts. This function must be connected
* by the user to an interrupt controller.
*
* The interrupts that are handled are:
*
* - Mode Fault Error. This interrupt is generated if this device is selected
*   as a slave when it is configured as a master. The driver aborts any data
*   transfer that is in progress by resetting FIFOs (if present) and resetting
*   its buffer pointers. The upper layer software is informed of the error.
*
* - Data Transmit Register (FIFO) Empty. This interrupt is generated when the
*   transmit register or FIFO is empty. The driver uses this interrupt during a
*   transmission to continually send/receive data until the transfer is done.
*
* - Data Transmit Register (FIFO) Underflow. This interrupt is generated when
*   the SPI device, when configured as a slave, attempts to read an empty
*   DTR/FIFO.  An empty DTR/FIFO usually means that software is not giving the
*   device data in a timely manner. No action is taken by the driver other than
*   to inform the upper layer software of the error.
*
* - Data Receive Register (FIFO) Overflow. This interrupt is generated when the
*   SPI device attempts to write a received byte to an already full DRR/FIFO.
*   A full DRR/FIFO usually means software is not emptying the data in a timely
*   manner.  No action is taken by the driver other than to inform the upper
*   layer software of the error.
*
* - Slave Mode Fault Error. This interrupt is generated if a slave device is
*   selected as a slave while it is disabled. No action is taken by the driver
*   other than to inform the upper layer software of the error.
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
*
* @return	None.
*
* @note
*
* The slave select register is being set to deselect the slave when a transfer
* is complete.  This is being done regardless of whether it is a slave or a
* master since the hardware does not drive the slave select as a slave.
*
******************************************************************************/
void XSpiPs_InterruptHandler(void *InstancePtr)
{
	XSpiPs *SpiPtr = (XSpiPs *)InstancePtr;
	u32 IntrStatus;
	u32 ConfigReg;
	unsigned BytesDone; /* Number of bytes done so far. */

	Xil_AssertVoid(InstancePtr != NULL);
	Xil_AssertVoid(SpiPtr->IsReady == XIL_COMPONENT_IS_READY);

	/*
	 * Immediately clear the interrupts in case the ISR causes another
	 * interrupt to be generated. If we clear at the end of the ISR,
	 * we may miss newly generated interrupts.
	 * Disable the TXOW interrupt because we transmit from within the ISR,
	 * which could potentially cause another TX_OW interrupt.
	 */
	IntrStatus =
		XSpiPs_ReadReg(SpiPtr->Config.BaseAddress, XSPIPS_SR_OFFSET);
	XSpiPs_WriteReg(SpiPtr->Config.BaseAddress, XSPIPS_SR_OFFSET,
			(IntrStatus & XSPIPS_IXR_WR_TO_CLR_MASK));
	XSpiPs_WriteReg(SpiPtr->Config.BaseAddress, XSPIPS_IDR_OFFSET,
			XSPIPS_IXR_TXOW_MASK);

	/*
	 * Check for mode fault error. We want to check for this error first,
	 * before checking for progress of a transfer, since this error needs
	 * to abort any operation in progress.
	 */
	if (XSPIPS_IXR_MODF_MASK == (IntrStatus & XSPIPS_IXR_MODF_MASK)) {
		BytesDone = SpiPtr->RequestedBytes - SpiPtr->RemainingBytes;

		/*
		 * Abort any operation currently in progress. This includes
		 * clearing the mode fault condition by reading the status
		 * register. Note that the status register should be read after
		 * the abort, since reading the status register clears the mode
		 * fault condition and would cause the device to restart any
		 * transfer that may be in progress.
		 */
		XSpiPs_Abort(SpiPtr);

		SpiPtr->StatusHandler(SpiPtr->StatusRef, XST_SPI_MODE_FAULT,
					BytesDone);

		return; /* Do not continue servicing other interrupts. */
	}


	if (IntrStatus & XSPIPS_IXR_TXOW_MASK) {
		u8 TempData;
		u32 TransCount;
		/*
		 * A transmit has just completed. Process received data and
		 * check for more data to transmit.
		 * First get the data received as a result of the transmit that
		 * just completed.  Always get the received data, but only fill
		 * the receive buffer if it is not null (it can be null when
		 * the device does not care to receive data).
		 * Initialize the TransCount based on the requested bytes.
		 * Loop on receive FIFO based on TransCount.
		 */
		TransCount = SpiPtr->RequestedBytes - SpiPtr->RemainingBytes;

		while (TransCount) {
			TempData = XSpiPs_RecvByte(SpiPtr->Config.BaseAddress);
			if (SpiPtr->RecvBufferPtr != NULL) {
				*SpiPtr->RecvBufferPtr++ = (u8) TempData;
			}
			SpiPtr->RequestedBytes--;
			--TransCount;
		}

		/*
		 * Fill the TXFIFO until data exists, otherwise fill upto
		 * FIFO depth.
		 */
		while ((SpiPtr->RemainingBytes > 0) &&
			(TransCount < XSPIPS_FIFO_DEPTH)) {
			XSpiPs_SendByte(SpiPtr->Config.BaseAddress,
					 *SpiPtr->SendBufferPtr);
			SpiPtr->SendBufferPtr++;
			SpiPtr->RemainingBytes--;
			++TransCount;
		}

		if ((SpiPtr->RemainingBytes == 0) &&
			(SpiPtr->RequestedBytes == 0)) {
			/*
			 * No more data to send. Disable the interrupt and
			 * inform the upper layer software that the transfer
			 * is done. The interrupt will be re-enabled when
			 * another transfer is initiated.
			 */
			XSpiPs_WriteReg(SpiPtr->Config.BaseAddress,
				 XSPIPS_IDR_OFFSET, XSPIPS_IXR_DFLT_MASK);

			/*
			 * Disable slave select lines as the transfer
			 * is complete.
			 */
			if (XSpiPs_IsManualChipSelect(InstancePtr)) {
				ConfigReg = XSpiPs_ReadReg(
					SpiPtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET);
				ConfigReg |= XSPIPS_CR_SSCTRL_MASK;
				XSpiPs_WriteReg(
					SpiPtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET, ConfigReg);
			}

			/*
			 * Clear the busy flag.
			 */
			SpiPtr->IsBusy = FALSE;

			/*
			 * Disable the device.
			 */
			XSpiPs_Disable(SpiPtr);

			/*
			 * Inform the Transfer done to upper layers.
			 */
			SpiPtr->StatusHandler(SpiPtr->StatusRef,
						XST_SPI_TRANSFER_DONE,
						SpiPtr->RequestedBytes);
		} else {
			/*
			 * Enable the TXOW interrupt.
			 */
			XSpiPs_WriteReg(SpiPtr->Config.BaseAddress,
				 XSPIPS_IER_OFFSET, XSPIPS_IXR_TXOW_MASK);
			/*
			 * Start the transfer by not inhibiting the transmitter
			 * any longer.
			 */
			if (XSpiPs_IsManualStart(SpiPtr)
				&& XSpiPs_IsMaster(SpiPtr)) {
				ConfigReg = XSpiPs_ReadReg(
					SpiPtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET);
				ConfigReg |= XSPIPS_CR_MANSTRT_MASK;
				XSpiPs_WriteReg(
					SpiPtr->Config.BaseAddress,
					 XSPIPS_CR_OFFSET, ConfigReg);
			}
		}
	}

	/*
	 * Check for overflow and underflow errors.
	 */
	if (IntrStatus & XSPIPS_IXR_RXOVR_MASK) {
		BytesDone = SpiPtr->RequestedBytes - SpiPtr->RemainingBytes;
		SpiPtr->IsBusy = FALSE;

		/*
		 * The Slave select lines are being manually controlled.
		 * Disable them because the transfer is complete.
		 */
		if (XSpiPs_IsManualChipSelect(SpiPtr)) {
			ConfigReg = XSpiPs_ReadReg(
				SpiPtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET);
			ConfigReg |= XSPIPS_CR_SSCTRL_MASK;
			XSpiPs_WriteReg(
				SpiPtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET, ConfigReg);
		}

		SpiPtr->StatusHandler(SpiPtr->StatusRef,
			XST_SPI_RECEIVE_OVERRUN, BytesDone);
	}

	if (IntrStatus & XSPIPS_IXR_TXUF_MASK) {
		BytesDone = SpiPtr->RequestedBytes - SpiPtr->RemainingBytes;

		SpiPtr->IsBusy = FALSE;
		/*
		 * The Slave select lines are being manually controlled.
		 * Disable them because the transfer is complete.
		 */
		if (XSpiPs_IsManualChipSelect(SpiPtr)) {
			ConfigReg = XSpiPs_ReadReg(
				SpiPtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET);
			ConfigReg |= XSPIPS_CR_SSCTRL_MASK;
			XSpiPs_WriteReg(
				SpiPtr->Config.BaseAddress,
				 XSPIPS_CR_OFFSET, ConfigReg);
		}

		SpiPtr->StatusHandler(SpiPtr->StatusRef,
			XST_SPI_TRANSMIT_UNDERRUN, BytesDone);
	}

}

/*****************************************************************************/
/**
*
* Aborts a transfer in progress by disabling the device and resetting the FIFOs
* if present. The byte counts are cleared, the busy flag is cleared, and mode
* fault is cleared.
*
* @param	InstancePtr is a pointer to the XSpiPs instance.
*
* @return	None.
*
* @note
*
* This function does a read/modify/write of the Config register. The user of
* this function needs to take care of critical sections.
*
******************************************************************************/
void XSpiPs_Abort(XSpiPs *InstancePtr)
{

	XSpiPs_Disable(InstancePtr);

	/*
	 * Clear the RX FIFO and drop any data.
	 */
	while ((XSpiPs_ReadReg(InstancePtr->Config.BaseAddress,
		 XSPIPS_SR_OFFSET) & XSPIPS_IXR_RXNEMPTY_MASK) ==
		XSPIPS_IXR_RXNEMPTY_MASK) {
		(void) XSpiPs_RecvByte(InstancePtr->Config.BaseAddress);
	}

	/*
	 * Clear mode fault condition.
	 */
	XSpiPs_WriteReg(InstancePtr->Config.BaseAddress,
			XSPIPS_SR_OFFSET,
			XSPIPS_IXR_MODF_MASK);

	InstancePtr->RemainingBytes = 0;
	InstancePtr->RequestedBytes = 0;
	InstancePtr->IsBusy = FALSE;
}

