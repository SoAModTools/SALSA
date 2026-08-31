import tkinter as tk
from tkinter import ttk

from SALSA.GUI.AnalysisPopup.analysis_view import AnalysisView


class AnalysisController:

    def __init__(self, view):
        self.view: AnalysisView = view

        # TODO - setup anything which might need a callback in within view

# Called when asked to export data
    def on_data_export(self, export_type='Ship battle turn data'):
        pass

    def perform_script_analysis(self):
        pass

    def add_analysis_to_export(self):
        pass

    def export_as_csv(self, csv_dict):
        pass

    def change_theme(self, theme):
        pass