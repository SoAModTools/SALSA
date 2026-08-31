
import os

path = os.path.dirname(__file__)
os.chdir(path)


# # for changing the taskbar icon, turn off for breakpoint in other threads
# if os.name == 'nt':
#     import ctypes
#     myappid = 'Chiichiis.SALSA.TomatoFree.1b'  # arbitrary string
#     ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(myappid)

if __name__ == '__main__':
    from SALSA.application import Application

    app = Application()
    app.mainloop()
