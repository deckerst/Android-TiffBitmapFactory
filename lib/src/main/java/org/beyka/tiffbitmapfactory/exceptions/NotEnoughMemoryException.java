package org.beyka.tiffbitmapfactory.exceptions;

/**
 * Created by alexeyba on 09.11.15.
 */
public class NotEnoughMemoryException extends RuntimeException {

    private int availableMemory;
    private int needMemory;

    public NotEnoughMemoryException(int availableMemory, int needMemory){
        super("Available memory is not enough to decode image. Available " + availableMemory + " bytes. Need " + needMemory + " bytes.");
        this.availableMemory = availableMemory;
        this.needMemory = needMemory;
    }

    public int getAvailableMemory() {
        return availableMemory;
    }

    public int getNeedMemory() {
        return needMemory;
    }
}
