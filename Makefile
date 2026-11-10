.PHONY: all clean bootstrap bt3
all: bt3

bt3:
	$(MAKE) -C port/ps3

bootstrap:
	$(MAKE) -C port/ps3/bootstrap

clean:
	$(MAKE) -C port/ps3 clean
	$(MAKE) -C port/ps3/bootstrap clean
